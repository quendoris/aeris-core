// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "aeris/source/adapter.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace aeris::source {

enum class ShapefileError : std::uint8_t {
    none = 0U,
    io_error,
    truncated_file,
    invalid_file_code,
    invalid_version,
    unsupported_shape_type,
    file_length_mismatch,
    malformed_record,
    invalid_coordinate,
    degenerate_ring,
    canonicalization_failed,
};

struct ShapefileRing final {
    geometry::LinearRing geometry;
    RingRole role = RingRole::exterior;
};

struct ShapefileRecord final {
    std::uint32_t record_number = 0U;
    std::vector<ShapefileRing> rings;
};

struct ShapefilePolygonResult final {
    std::vector<ShapefileRecord> records;
    ShapefileError error = ShapefileError::none;
    geometry::GeographicError geographic_error = geometry::GeographicError::none;
    std::uint32_t failed_record_number = 0U;
    std::string diagnostic;

    [[nodiscard]] bool ok() const noexcept {
        return error == ShapefileError::none;
    }
};

// Minimal, strict reader for ESRI Shapefile Polygon (shape type 5).
// It intentionally does not parse DBF attributes or attempt to support every
// Shapefile shape family. Provider adapters compose this geometry reader with
// their own metadata/attribute handling as required.
[[nodiscard]] ShapefilePolygonResult read_polygon_shapefile(
    const std::filesystem::path& path
);

}  // namespace aeris::source
