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
    GlobePolygonOptions initial{};

    // Orthographic screen area is NOT compared with WGS84 geographic area.
    // These values constrain convergence only between consecutive finite
    // approximations of the same visible view-space region.
    double relative_area_stability_tolerance = 5e-3;
    double absolute_area_stability_tolerance_m2 = 1.0;

    unsigned max_refinement_rounds = 12U;
};

struct VerifiedGlobePolygonResult final {
    GlobePolygonResult polygon{};

    // Aggregate screen-area convergence observable.
    double estimated_planar_area_error_m2 = 0.0;
    double allowed_planar_area_delta_m2 = 0.0;

    // Largest absolute per-component area change between the two accepted
    // consecutive approximations. Each component is independently required to
    // satisfy its own absolute/relative/binary64 convergence budget; this field
    // is telemetry, not a substitute for that per-component check.
    double estimated_max_component_area_error_m2 = 0.0;

    // A derived component whose absolute screen area is at or below this
    // conservative binary64 resolution floor has no numerically meaningful
    // orientation. It remains in the finite output but is classified as
    // negligible instead of being assigned a fabricated stable sign.
    double component_area_resolution_floor_m2 = 0.0;
    std::size_t significant_component_count = 0U;
    std::size_t negligible_component_count = 0U;

    unsigned refinement_rounds = 0U;
    double final_curve_geometric_tolerance_m = 0.0;
    double final_horizon_arc_tolerance_m = 0.0;

    bool topology_stable = false;
    bool component_orientation_stable = false;
    bool component_area_stable = false;

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
//    significant/negligible classification are stable across consecutive
//    acceptable refinements;
// 3. every derived component's signed screen area converges within its own
//    absolute/relative/binary64 budget; and
// 4. aggregate signed orthographic planar area also converges within the
//    declared stability budget.
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
