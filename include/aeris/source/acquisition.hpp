// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aeris::source {

enum class AcquisitionError : std::uint8_t {
    none = 0U,
    invalid_manifest,
    unsafe_resource_path,
    duplicate_resource,
    missing_resource,
    unexpected_resource_type,
    size_mismatch,
    hash_mismatch,
    io_error,
};

struct ResourceSpec final {
    std::string logical_name;
    std::filesystem::path relative_path;
    std::string sha256;
    std::optional<std::uintmax_t> size_bytes;
};

struct SnapshotManifest final {
    std::string provider;
    std::string dataset;
    std::string snapshot;
    std::string source_uri;
    std::string retrieved_at_utc;
    std::vector<ResourceSpec> resources;
};

struct SnapshotVerificationResult;

class VerifiedSnapshot final {
public:
    VerifiedSnapshot(const VerifiedSnapshot&) = default;
    VerifiedSnapshot(VerifiedSnapshot&&) noexcept = default;
    VerifiedSnapshot& operator=(const VerifiedSnapshot&) = default;
    VerifiedSnapshot& operator=(VerifiedSnapshot&&) noexcept = default;

    [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }
    [[nodiscard]] const SnapshotManifest& manifest() const noexcept { return manifest_; }
    [[nodiscard]] const std::string& content_sha256() const noexcept { return content_sha256_; }

    [[nodiscard]] std::optional<std::filesystem::path> resource_path(
        std::string_view logical_name
    ) const;

private:
    friend SnapshotVerificationResult verify_local_snapshot(
        const std::filesystem::path&,
        const SnapshotManifest&
    );

    VerifiedSnapshot(
        std::filesystem::path root,
        SnapshotManifest manifest,
        std::string content_sha256
    );

    std::filesystem::path root_;
    SnapshotManifest manifest_;
    std::string content_sha256_;
};

struct SnapshotVerificationResult final {
    std::optional<VerifiedSnapshot> snapshot;
    AcquisitionError error = AcquisitionError::none;
    std::string failed_resource;
    std::string diagnostic;

    [[nodiscard]] bool ok() const noexcept {
        return error == AcquisitionError::none && snapshot.has_value();
    }
};

// Verifies an already materialized local snapshot. Network access, archive
// extraction, cache lookup, USB copying, and other transport mechanisms are
// deliberately outside this function. Every acquisition backend must end by
// producing bytes that pass this same verification boundary.
[[nodiscard]] SnapshotVerificationResult verify_local_snapshot(
    const std::filesystem::path& root,
    const SnapshotManifest& manifest
);

}  // namespace aeris::source
