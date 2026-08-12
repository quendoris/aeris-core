// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstddef>
#include <vector>

namespace aeris::geometry {

struct GeodeticPoint final {
    double longitude_rad = 0.0;
    double latitude_rad = 0.0;
};

enum class GeographicError {
    none = 0,
    too_few_vertices,
    non_finite_coordinate,
    latitude_out_of_range,
    ambiguous_half_turn,
    longitude_winding_unsupported,
    invalid_options,
    numerical_domain_error,
    integration_limit_exceeded,
};

struct LinearRing final {
    std::vector<GeodeticPoint> vertices;
    double closing_longitude_rad = 0.0;
    int longitude_winding = 0;
};

struct LinearRingResult final {
    LinearRing value{};
    GeographicError error = GeographicError::none;

    [[nodiscard]] bool ok() const noexcept {
        return error == GeographicError::none;
    }
};

struct GeographicAreaOptions final {
    // Dimensionless absolute tolerance for integral_0^1 q(phi(t)) dt on
    // each canonical edge. The resulting area error estimate is reported
    // in square metres after longitude and WGS84 scale are applied.
    double edge_integral_tolerance = 1e-14;
    unsigned max_integration_depth = 24U;
};

struct GeographicAreaResult final {
    double signed_area_m2 = 0.0;
    double estimated_abs_error_m2 = 0.0;
    unsigned deepest_integration_level = 0U;
    GeographicError error = GeographicError::none;

    [[nodiscard]] constexpr bool ok() const noexcept {
        return error == GeographicError::none;
    }
};

[[nodiscard]] LinearRingResult canonicalize_wgs84_linear_ring(
    const GeodeticPoint* points,
    std::size_t count
);

[[nodiscard]] inline LinearRingResult canonicalize_wgs84_linear_ring(
    const std::vector<GeodeticPoint>& points
) {
    return canonicalize_wgs84_linear_ring(points.data(), points.size());
}

[[nodiscard]] GeodeticPoint interpolate_wgs84_linear_edge(
    GeodeticPoint start,
    GeodeticPoint end,
    double parameter
) noexcept;

[[nodiscard]] GeographicAreaResult signed_wgs84_linear_ring_area(
    const LinearRing& ring,
    const GeographicAreaOptions& options = {}
) noexcept;

}  // namespace aeris::geometry
