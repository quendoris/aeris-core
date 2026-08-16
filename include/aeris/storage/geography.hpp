// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "aeris/storage/project.hpp"
#include "aeris/storage/provenance.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace aeris::storage {

// Storage-owned enums deliberately do not depend on source/geometry enums.
// Their numeric values are part of the draft project schema generation.
enum class GeographicRingRole : std::uint8_t {
    exterior = 0U,
    interior = 1U,
};

enum class GeographicInteriorSide : std::uint8_t {
    unspecified = 0U,
    left = 1U,
    right = 2U,
};

struct GeographicPointRecord {
    double longitude_rad{0.0};
    double latitude_rad{0.0};
};

struct GeographicRingRecord {
    GeographicRingRole role{GeographicRingRole::exterior};
    GeographicInteriorSide interior_side{GeographicInteriorSide::unspecified};
    double closing_longitude_rad{0.0};
    std::int32_t longitude_winding{0};
    std::vector<GeographicPointRecord> vertices;
};

struct SourceFeatureRecord {
    std::string stable_id;
    std::string source_id;
    std::vector<GeographicRingRecord> rings;
};

struct SourceDatasetRecord {
    SourceSnapshotRecord source;
    std::vector<SourceFeatureRecord> features;
};

struct SourceDatasetMutationResult {
    Status status;
    bool inserted{false};
    bool durably_committed{false};

    [[nodiscard]] bool ok() const noexcept { return status.ok(); }
};

struct SourceFeatureListResult {
    Status status;
    std::vector<SourceFeatureRecord> records;

    [[nodiscard]] bool ok() const noexcept { return status.ok(); }
};

// Stores source provenance, verified resource identity, and canonical geographic
// features as one acknowledged project mutation. Exact retry compares the full
// immutable dataset, not provenance alone.
[[nodiscard]] SourceDatasetMutationResult store_source_dataset(
    ProjectStore& project,
    const SourceDatasetRecord& record,
    std::string_view modified_utc);

// Convenience materializing reader for one source. High-volume streaming reads
// are a separate future API; this function still validates every stored ring.
[[nodiscard]] SourceFeatureListResult list_source_features(
    const ProjectStore& project,
    std::string_view source_id);

}  // namespace aeris::storage
