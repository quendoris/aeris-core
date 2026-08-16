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

struct SourceResourceRecord {
    std::string logical_name;
    std::string sha256;
    std::optional<std::uint64_t> size_bytes;
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
    std::string retrieved_at_utc;
    std::string worldview;
    std::vector<SourceResourceRecord> resources;
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

[[nodiscard]] SourceSnapshotMutationResult store_source_snapshot(
    ProjectStore& project,
    const SourceSnapshotRecord& record,
    std::string_view modified_utc);

[[nodiscard]] SourceSnapshotListResult list_source_snapshots(const ProjectStore& project);

}  // namespace aeris::storage
