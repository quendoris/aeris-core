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

inline constexpr std::string_view kCanonicalGeometryModelId =
    "aeris.geometry.wgs84-linear-ring.v1";
inline constexpr std::string_view kCanonicalCoordinateEncodingId =
    "aeris.coord.ieee754-binary64-le-radians.v1";

enum class StoredRingRole : std::uint8_t {
    exterior = 0U,
    interior = 1U,
};

enum class StoredInteriorSide : std::uint8_t {
    unspecified = 0U,
    left = 1U,
    right = 2U,
};

struct GeographicPointRecord final {
    double longitude_rad = 0.0;
    double latitude_rad = 0.0;
};

struct GeographicRingRecord final {
    StoredRingRole role = StoredRingRole::exterior;
    StoredInteriorSide interior_side = StoredInteriorSide::unspecified;
    std::int32_t longitude_winding = 0;
    double closing_longitude_rad = 0.0;
    std::vector<GeographicPointRecord> vertices;
};

struct FeatureGeometryRecord final {
    std::string stable_id;
    std::string source_feature_id;
    std::vector<GeographicRingRecord> rings;
};

struct SourceGeometryRecord final {
    std::string source_id;
    std::vector<FeatureGeometryRecord> features;
};

struct SourceGeometryMutationResult final {
    Status status;
    bool inserted{false};
    bool durably_committed{false};

    [[nodiscard]] bool ok() const noexcept { return status.ok(); }
};

struct FeatureGeometryIndexEntry final {
    std::string stable_id;
    std::string source_feature_id;
    std::uint32_t ring_count{0U};
};

struct SourceGeometryIndexResult final {
    Status status;
    std::vector<FeatureGeometryIndexEntry> features;

    [[nodiscard]] bool ok() const noexcept { return status.ok(); }
};

struct FeatureGeometryLoadResult final {
    Status status;
    std::optional<FeatureGeometryRecord> feature;

    [[nodiscard]] bool ok() const noexcept {
        return status.ok() && feature.has_value();
    }
};

// Records one immutable canonical geometry set for an already-persisted source.
// Exact retries are idempotent. A different geometry set under the same source
// ID is rejected rather than silently replacing canonical project truth.
[[nodiscard]] SourceGeometryMutationResult store_source_geometry(
    ProjectStore& project,
    const SourceGeometryRecord& record,
    std::string_view modified_utc);

// Metadata-only enumeration avoids decoding every coordinate blob when callers
// only need stable feature identities or a lazy loading plan.
[[nodiscard]] SourceGeometryIndexResult list_source_geometry_index(
    const ProjectStore& project,
    std::string_view source_id);

// Loads and validates one feature's canonical WGS84 geometry.
[[nodiscard]] FeatureGeometryLoadResult load_feature_geometry(
    const ProjectStore& project,
    std::string_view source_id,
    std::string_view stable_id);

}  // namespace aeris::storage
