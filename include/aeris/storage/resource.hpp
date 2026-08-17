// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "aeris/storage/project.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace aeris::storage {

// Canonical embedded-resource chunking is deliberately bounded and streaming-
// friendly. The limit is an implementation/security bound, not a render-quality
// or product-tier ceiling.
inline constexpr std::size_t kEmbeddedResourceChunkBytes = 1024U * 1024U;
inline constexpr std::size_t kMaxEmbeddedResourceChunks = 4U * 1024U * 1024U;

enum class ResourceStorageMode : std::uint8_t {
    external = 0U,
    embedded = 1U,
};

struct ProjectResourceIdentity final {
    std::string resource_id;
    std::string sha256;
    std::string media_type;
    std::uint64_t size_bytes{0U};

    // Portable retrieval identity only. This is never a cache-local path and
    // may be empty when no network locator is semantically available.
    std::string retrieval_uri;

    // Frozen projects require every resource carrying this flag to be embedded
    // and hash-verified inside the .aeris file.
    bool required_for_reproduction{true};
};

struct ProjectResourceRecord final {
    ProjectResourceIdentity identity;
    ResourceStorageMode storage_mode{ResourceStorageMode::external};
    std::uint64_t chunk_count{0U};
};

struct ResourceMutationResult final {
    Status status;
    bool inserted{false};
    bool representation_changed{false};
    bool durably_committed{false};

    [[nodiscard]] bool ok() const noexcept { return status.ok(); }
};

struct ProjectResourceListResult final {
    Status status;
    std::vector<ProjectResourceRecord> records;

    [[nodiscard]] bool ok() const noexcept { return status.ok(); }
};

struct ProjectFreezeResult final {
    Status status;
    bool changed{false};
    bool durably_committed{false};

    [[nodiscard]] bool ok() const noexcept { return status.ok(); }
};

// Records immutable content identity without storing a machine-local path.
// If an identical embedded resource already exists, this is an idempotent no-op
// and never downgrades the stronger embedded representation.
[[nodiscard]] ResourceMutationResult store_external_resource(
    ProjectStore& project,
    const ProjectResourceIdentity& resource,
    std::string_view modified_utc);

// Streams one local input file into canonical fixed-size SQLite chunks while
// recomputing both per-chunk and aggregate SHA-256. The input path is an API
// source only and is never persisted in the project. Existing identical external
// identity may be upgraded to embedded atomically.
[[nodiscard]] ResourceMutationResult embed_resource_file(
    ProjectStore& project,
    const ProjectResourceIdentity& resource,
    const std::filesystem::path& input_path,
    std::string_view modified_utc);

[[nodiscard]] ProjectResourceListResult list_project_resources(
    const ProjectStore& project);

// Visits verified embedded bytes in canonical chunk order without materializing
// the complete resource in memory. Stored chunk hashes and the aggregate resource
// hash are rechecked before success is returned.
using EmbeddedResourceConsumer = std::function<Status(const void*, std::size_t)>;
[[nodiscard]] Status stream_embedded_resource(
    const ProjectStore& project,
    std::string_view resource_id,
    const EmbeddedResourceConsumer& consumer);

// Verifies that every resource required for reproduction is embedded and
// content-valid, then marks the current project revision portable/frozen in one
// acknowledged metadata mutation. Already-valid frozen state is idempotent.
[[nodiscard]] ProjectFreezeResult freeze_project(
    ProjectStore& project,
    std::string_view modified_utc);

}  // namespace aeris::storage
