// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "aeris/storage/project.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aeris::storage {

enum class SourceMaterializationState : std::uint8_t {
    referenced = 0U,
    materialized = 1U,
};

struct SourceResourceRecord {
    std::string logical_name;
    std::string sha256;
    std::optional<std::uint64_t> size_bytes;

    // Canonical portable path expected by the adapter inside a verified local
    // snapshot root. Empty means the project knows immutable content identity
    // but does not contain an automatic acquisition recipe for this resource.
    std::string relative_path;

    // Portable retrieval locator only. Never a machine-local cache path. Empty
    // is valid for manually supplied/offline references; non-empty values are
    // validated as URI-shaped locators and combine with relative_path/hash/size
    // to make a source automatically fetchable.
    std::string retrieval_uri;
};

struct SourceSnapshotRecord {
    std::string source_id;
    std::string adapter_id;
    std::uint32_t capability_bits{0U};
    std::uint8_t temporal_class{0U};
    std::string provider;
    std::string dataset;
    std::string snapshot;
    std::string dataset_version;
    std::string source_uri;
    std::string license_id;
    std::string content_sha256;

    // Referenced sources may leave this empty because merely recording an exact
    // acquisition identity is not a byte retrieval event. Materialized sources
    // require a canonical UTC timestamp for the verified bytes that produced
    // their canonical project content. Exact materialization retries do not
    // rewrite it merely because the same bytes were fetched again later.
    std::string retrieved_at_utc;

    std::string worldview;
    std::vector<SourceResourceRecord> resources;
    SourceMaterializationState materialization_state{SourceMaterializationState::referenced};
};

struct SourceSnapshotMutationResult {
    Status status;
    bool inserted{false};
    bool durably_committed{false};

    [[nodiscard]] bool ok() const noexcept { return status.ok(); }
};

struct SourceSnapshotListResult {
    Status status;
    std::vector<SourceSnapshotRecord> records;

    [[nodiscard]] bool ok() const noexcept { return status.ok(); }
};

[[nodiscard]] bool is_canonical_sha256(std::string_view value) noexcept;

// True only when every declared resource contains a canonical portable relative
// path and retrieval URI, so an acquisition backend can obtain bytes without
// consulting machine-local state. This does not perform network access.
[[nodiscard]] bool source_reference_is_fetchable(
    const SourceSnapshotRecord& record) noexcept;

// Stores immutable source identity/acquisition recipe without canonical decoded
// content. New records are always Referenced. Exact retries are idempotent; an
// already-materialized identical source is never demoted by this lower-level
// call. Use store_source_dataset() to atomically materialize verified content.
[[nodiscard]] SourceSnapshotMutationResult store_source_snapshot(
    ProjectStore& project,
    const SourceSnapshotRecord& record,
    std::string_view modified_utc);

[[nodiscard]] SourceSnapshotListResult list_source_snapshots(const ProjectStore& project);

}  // namespace aeris::storage
