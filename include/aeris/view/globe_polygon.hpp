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
// The returned rings are one finite approximation at the requested tolerances.
// Canonical WGS84 source vertices and source provenance are not mutated.
[[nodiscard]] GlobePolygonResult project_visible_wgs84_linear_polygon_ring(
    const geometry::LinearRing& ring,
    const geo::Mat3& world_to_view,
    const GlobePolygonOptions& options = {},
    double radius_m = geo::authalic_radius_m()
);

enum class VerifiedGlobePolygonError {
    none = 0,
    invalid_options,
    finite_projection_failed,
    component_orientation_unstable,
    topology_unstable,
    area_convergence_unmet,
};

struct VerifiedGlobePolygonOptions final {
    // First finite approximation. Verification halves the curve and horizon
    // arc geometric tolerances after every unsuccessful refinement round.
    GlobePolygonOptions initial{};

    // Orthographic screen area is NOT compared with WGS84 geographic area.
    // It is used only as a convergence observable between consecutive finite
    // approximations of the same visible region.
    double relative_area_stability_tolerance = 5e-3;
    double absolute_area_stability_tolerance_m2 = 1.0;

    // At least two successful finite approximations are required before a
    // result can be declared verified.
    unsigned max_refinement_rounds = 12U;
};

struct VerifiedGlobePolygonResult final {
    GlobePolygonResult polygon{};

    // Conservative observable error estimate: the absolute signed planar-area
    // difference between the two consecutive stable approximations that
    // satisfied the verification contract.
    double estimated_planar_area_error_m2 = 0.0;
    double allowed_planar_area_delta_m2 = 0.0;

    // A derived visible component whose absolute screen area is at or below
    // this binary64 resolution floor has no numerically meaningful orientation.
    // Such a component remains represented by the finite projector, but the
    // verifier classifies it as negligible instead of inventing a stable sign.
    double component_area_resolution_floor_m2 = 0.0;
    std::size_t significant_component_count = 0U;
    std::size_t negligible_component_count = 0U;

    unsigned refinement_rounds = 0U;
    double final_curve_geometric_tolerance_m = 0.0;
    double final_horizon_arc_tolerance_m = 0.0;

    bool topology_stable = false;
    bool component_orientation_stable = false;

    VerifiedGlobePolygonError error = VerifiedGlobePolygonError::none;

    [[nodiscard]] bool ok() const noexcept {
        return error == VerifiedGlobePolygonError::none && polygon.ok();
    }
};

// Verified high-level fill projection. The finite polygon projector remains a
// deterministic one-shot primitive; this wrapper repeatedly refines it until:
//
// 1. every numerically significant partial-horizon component has the
//    orientation required by RingInteriorSide; components below the explicit
//    binary64 screen-area resolution floor are classified as negligible rather
//    than assigned an unreliable sign;
// 2. horizon-crossing count, output-component count, and per-component
//    significant/negligible classification are stable across two consecutive
//    acceptable refinements; and
// 3. signed orthographic planar area converges within the declared stability
//    budget.
//
// The geometric and horizon-arc tolerances are halved together on each round.
// Horizon root tolerance and resource ceilings are not silently relaxed.
// Orthographic area is only a numerical convergence signal and is never treated
// as equal to geographic WGS84 area.
[[nodiscard]] VerifiedGlobePolygonResult
project_visible_wgs84_linear_polygon_ring_verified(
    const geometry::LinearRing& ring,
    const geo::Mat3& world_to_view,
    const VerifiedGlobePolygonOptions& options = {},
    double radius_m = geo::authalic_radius_m()
);

}  // namespace aeris::view
