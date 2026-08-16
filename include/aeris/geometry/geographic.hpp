// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
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
    noncanonical_ring,
};

// A closed curve on a sphere separates two valid regions. For ordinary local
// rings the intended side is often obvious from source topology, but for
// polar/winding rings it must be explicit. "left" and "right" are relative
// to traversal order on the oriented WGS84 longitude/latitude surface.
enum class RingInteriorSide : std::uint8_t {
    unspecified = 0U,
    left,
    right,
};

struct LinearRing final {
    std::vector<GeodeticPoint> vertices;
    double closing_longitude_rad = 0.0;
    int longitude_winding = 0;
    RingInteriorSide interior_side = RingInteriorSide::unspecified;
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

// Verifies the invariants of an already-canonical ring without rebuilding it.
// This is intentionally distinct from canonicalize_wgs84_linear_ring(): the
// constructor uses accumulated binary64 arithmetic, so bitwise idempotence of a
// second construction pass is not itself a canonicality requirement.
[[nodiscard]] GeographicError validate_canonical_wgs84_linear_ring(
    const LinearRing& ring
) noexcept;

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
