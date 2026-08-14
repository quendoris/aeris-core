// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "aeris/geometry/geographic.hpp"
#include "aeris/geometry/planar.hpp"
#include "aeris/view/globe_curve.hpp"

#include <cstddef>
#include <vector>

namespace aeris::view {

enum class GlobePolygonError {
    none = 0,
    invalid_options,
    invalid_geometry,
    missing_interior_side,
    geographic_area_failed,
    curve_failed,
    ambiguous_hemisphere_coverage,
    ambiguous_horizon_topology,
    horizon_arc_limit_exceeded,
    output_ring_limit_exceeded,
    non_finite_planar_area,
    planar_area_out_of_range,
    orientation_mismatch,
};

struct GlobePolygonOptions final {
    GlobeCurveOptions curve{};
    double horizon_arc_tolerance_m = 100.0;
    std::size_t max_horizon_arc_segments = 1'000'000U;
    std::size_t max_output_rings = 4096U;
};

struct GlobePolygonResult final {
    // Closed planar rings represented without a duplicate terminal copy of the
    // first point. Signed orientation follows the canonical source region:
    // left-side regions have positive aggregate planar area, right-side
    // regions negative aggregate planar area.
    std::vector<std::vector<geometry::PlanarPoint>> rings;

    double source_signed_area_m2 = 0.0;
    double planar_signed_area_m2 = 0.0;
    double visible_disk_area_m2 = 0.0;

    std::size_t horizon_crossings = 0U;
    std::size_t horizon_arc_segments = 0U;
    std::size_t projected_vertices = 0U;

    GlobePolygonError error = GlobePolygonError::none;
    geometry::GeographicError geographic_error = geometry::GeographicError::none;
    GlobeCurveError curve_error = GlobeCurveError::none;
    geo::MathError sample_error = geo::MathError::none;

    [[nodiscard]] bool ok() const noexcept {
        return error == GlobePolygonError::none;
    }
};

// Intersect one canonical geographic region with the visible hemisphere of an
// authalic orthographic globe. RingInteriorSide is mandatory: horizon arcs are
// topology, not styling, and their direction depends on the intended source
// interior.
//
// The returned rings are derived planar fill geometry. Canonical WGS84 source
// vertices and source provenance are not mutated.
[[nodiscard]] GlobePolygonResult project_visible_wgs84_linear_polygon_ring(
    const geometry::LinearRing& ring,
    const geo::Mat3& world_to_view,
    const GlobePolygonOptions& options = {},
    double radius_m = geo::authalic_radius_m()
);

}  // namespace aeris::view
