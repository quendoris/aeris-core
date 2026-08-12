// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/source/acquisition.hpp"

#include "aeris/util/sha256.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace aeris::source {
namespace {

[[nodiscard]] bool contains_nul(const std::string& value) noexcept {
    return value.find('\0') != std::string::npos;
}

[[nodiscard]] bool canonical_sha256(const std::string& value) noexcept {
    if (value.size() != 64U) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](const char character) {
        return (character >= '0' && character <= '9') ||
               (character >= 'a' && character <= 'f');
    });
}

[[nodiscard]] bool safe_relative_path(const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory()) {
        return false;
    }

    const std::filesystem::path normalized = path.lexically_normal();
    if (normalized.empty() || normalized == std::filesystem::path(".")) {
        return false;
    }
    for (const auto& component : normalized) {
        if (component == std::filesystem::path("..")) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool path_is_within(
    const std::filesystem::path& root,
    const std::filesystem::path& candidate
) noexcept {
    auto root_it = root.begin();
    auto candidate_it = candidate.begin();
    while (root_it != root.end()) {
        if (candidate_it == candidate.end() || *root_it != *candidate_it) {
            return false;
        }
        ++root_it;
        ++candidate_it;
    }
    return true;
}

[[nodiscard]] bool hash_u64(util::Sha256& hash, const std::uint64_t value) noexcept {
    std::array<unsigned char, 8> bytes{};
    for (unsigned index = 0U; index < 8U; ++index) {
        bytes[index] = static_cast<unsigned char>(
            (value >> ((7U - index) * 8U)) & 0xffU
        );
    }
    return hash.update(bytes.data(), bytes.size()) == util::HashError::none;
}

[[nodiscard]] bool hash_string(util::Sha256& hash, const std::string& value) noexcept {
    return hash_u64(hash, static_cast<std::uint64_t>(value.size())) &&
           hash.update(value.data(), value.size()) == util::HashError::none;
}

struct VerifiedResource final {
    ResourceSpec spec;
    std::filesystem::path canonical_path;
    std::uintmax_t actual_size = 0U;
};

[[nodiscard]] std::optional<std::string> aggregate_content_hash(
    std::vector<VerifiedResource> resources
) {
    std::sort(resources.begin(), resources.end(), [](const auto& left, const auto& right) {
        return left.spec.logical_name < right.spec.logical_name;
    });

    util::Sha256 hash{};
    constexpr char domain[] = "AERIS-SNAPSHOT-CONTENT-v1";
    if (hash.update(domain, sizeof(domain) - 1U) != util::HashError::none ||
        !hash_u64(hash, static_cast<std::uint64_t>(resources.size()))) {
        return std::nullopt;
    }

    for (const VerifiedResource& resource : resources) {
        const std::string portable_path = resource.spec.relative_path.lexically_normal().generic_string();
        if (!hash_string(hash, resource.spec.logical_name) ||
            !hash_string(hash, portable_path) ||
            !hash_string(hash, resource.spec.sha256) ||
            !hash_u64(hash, static_cast<std::uint64_t>(resource.actual_size))) {
            return std::nullopt;
        }
    }

    return hash.finalize().hex();
}

[[nodiscard]] SnapshotVerificationResult failure(
    const AcquisitionError error,
    std::string failed_resource,
    std::string diagnostic
) {
    SnapshotVerificationResult result{};
    result.error = error;
    result.failed_resource = std::move(failed_resource);
    result.diagnostic = std::move(diagnostic);
    return result;
}

}  // namespace

VerifiedSnapshot::VerifiedSnapshot(
    std::filesystem::path root,
    SnapshotManifest manifest,
    std::string content_sha256
)
    : root_(std::move(root)),
      manifest_(std::move(manifest)),
      content_sha256_(std::move(content_sha256)) {}

std::optional<std::filesystem::path> VerifiedSnapshot::resource_path(
    const std::string_view logical_name
) const {
    for (const ResourceSpec& resource : manifest_.resources) {
        if (resource.logical_name == logical_name) {
            return root_ / resource.relative_path.lexically_normal();
        }
    }
    return std::nullopt;
}

SnapshotVerificationResult verify_local_snapshot(
    const std::filesystem::path& root,
    const SnapshotManifest& manifest
) {
    if (manifest.provider.empty() || manifest.dataset.empty() || manifest.snapshot.empty() ||
        manifest.source_uri.empty() || manifest.retrieved_at_utc.empty() || manifest.resources.empty() ||
        contains_nul(manifest.provider) || contains_nul(manifest.dataset) ||
        contains_nul(manifest.snapshot) || contains_nul(manifest.source_uri) ||
        contains_nul(manifest.retrieved_at_utc)) {
        return failure(AcquisitionError::invalid_manifest, {}, "snapshot manifest identity is incomplete");
    }

    std::error_code error_code{};
    const std::filesystem::path canonical_root = std::filesystem::canonical(root, error_code);
    if (error_code || !std::filesystem::is_directory(canonical_root, error_code) || error_code) {
        return failure(AcquisitionError::io_error, {}, "snapshot root is not a readable directory");
    }

    std::set<std::string> logical_names;
    std::set<std::string> relative_paths;
    std::vector<VerifiedResource> verified;
    verified.reserve(manifest.resources.size());

    for (const ResourceSpec& resource : manifest.resources) {
        if (resource.logical_name.empty() || contains_nul(resource.logical_name) ||
            !canonical_sha256(resource.sha256) || !safe_relative_path(resource.relative_path)) {
            return failure(
                !safe_relative_path(resource.relative_path)
                    ? AcquisitionError::unsafe_resource_path
                    : AcquisitionError::invalid_manifest,
                resource.logical_name,
                "resource manifest entry is invalid"
            );
        }

        const std::string normalized_path = resource.relative_path.lexically_normal().generic_string();
        if (!logical_names.insert(resource.logical_name).second ||
            !relative_paths.insert(normalized_path).second) {
            return failure(
                AcquisitionError::duplicate_resource,
                resource.logical_name,
                "resource logical name or relative path is duplicated"
            );
        }

        const std::filesystem::path requested_path = canonical_root / resource.relative_path.lexically_normal();
        const std::filesystem::path canonical_resource =
            std::filesystem::canonical(requested_path, error_code);
        if (error_code) {
            return failure(
                AcquisitionError::missing_resource,
                resource.logical_name,
                "required snapshot resource does not exist"
            );
        }
        if (!path_is_within(canonical_root, canonical_resource)) {
            return failure(
                AcquisitionError::unsafe_resource_path,
                resource.logical_name,
                "resource resolves outside snapshot root"
            );
        }
        if (!std::filesystem::is_regular_file(canonical_resource, error_code) || error_code) {
            return failure(
                AcquisitionError::unexpected_resource_type,
                resource.logical_name,
                "snapshot resource is not a regular file"
            );
        }

        const std::uintmax_t actual_size = std::filesystem::file_size(canonical_resource, error_code);
        if (error_code) {
            return failure(AcquisitionError::io_error, resource.logical_name, "unable to read resource size");
        }
        if (resource.size_bytes.has_value() && actual_size != *resource.size_bytes) {
            return failure(AcquisitionError::size_mismatch, resource.logical_name, "resource size does not match manifest");
        }

        const util::Sha256FileResult hash = util::sha256_file(canonical_resource);
        if (!hash.ok()) {
            return failure(AcquisitionError::io_error, resource.logical_name, "unable to hash snapshot resource");
        }
        if (hash.digest.hex() != resource.sha256) {
            return failure(AcquisitionError::hash_mismatch, resource.logical_name, "resource SHA-256 does not match manifest");
        }

        verified.push_back({resource, canonical_resource, actual_size});
    }

    const std::optional<std::string> content_hash = aggregate_content_hash(verified);
    if (!content_hash.has_value()) {
        return failure(AcquisitionError::io_error, {}, "unable to compute aggregate snapshot content hash");
    }

    SnapshotVerificationResult result{};
    result.snapshot = VerifiedSnapshot(canonical_root, manifest, *content_hash);
    return result;
}

}  // namespace aeris::source
