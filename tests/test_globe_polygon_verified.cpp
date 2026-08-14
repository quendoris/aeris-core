// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/view/globe_polygon.hpp"

#include "aeris/geo/wgs84.hpp"

#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

[[nodiscard]] double radians(const double degrees) {
    return degrees * aeris::geo::kPi / 180.0;
}

[[nodiscard]] aeris::geometry::LinearRing make_ring(
    const std::vector<aeris::geometry::GeodeticPoint>& points,
    const aeris::geometry::RingInteriorSide side
) {
    auto canonical = aeris::geometry::canonicalize_wgs84_linear_ring(points);
    if (!canonical.ok()) {
        return {};
    }
    canonical.value.interior_side = side;
    return canonical.value;
}

[[nodiscard]] aeris::view::VerifiedGlobePolygonOptions coarse_verified_options() {
    aeris::view::VerifiedGlobePolygonOptions options{};
    options.initial.curve.geometric_tolerance_m = 5'000.0;
    options.initial.curve.horizon_tolerance_m = 0.01;
    options.initial.curve.max_subdivision_depth = 32U;
    options.initial.curve.max_root_iterations = 80U;
    options.initial.curve.max_segments = 1'000'000U;
    options.initial.horizon_arc_tolerance_m = 500.0;
    options.initial.max_horizon_arc_segments = 1'000'000U;
    options.initial.max_output_rings = 4096U;
    options.relative_area_stability_tolerance = 5e-3;
    options.absolute_area_stability_tolerance_m2 = 1.0;
    options.max_refinement_rounds = 18U;
    return options;
}

bool test_thin_horizon_sliver_refines() {
    // On the identity orthographic camera the horizon is lambda = +/-90 deg.
    // The intended region is only 0.1 deg inside that horizon, so its visible
    // screen footprint is extremely thin and coarse finite chords are a useful
    // stress case for orientation and area-stability verification.
    const auto ring = make_ring(
        {
            {radians(89.9), radians(-10.0)},
            {radians(100.0), radians(-10.0)},
            {radians(100.0), radians(10.0)},
            {radians(89.9), radians(10.0)},
        },
        aeris::geometry::RingInteriorSide::left
    );
    if (ring.vertices.size() != 4U) {
        std::cerr << "thin sliver fixture failed to canonicalize\n";
        return false;
    }

    const auto options = coarse_verified_options();
    const auto result =
        aeris::view::project_visible_wgs84_linear_polygon_ring_verified(
            ring,
            aeris::geo::Mat3{},
            options,
            aeris::geo::authalic_radius_m()
        );

    if (!result.ok()) {
        std::cerr
            << "thin sliver verification failed: error="
            << static_cast<int>(result.error)
            << " finite_error=" << static_cast<int>(result.polygon.error)
            << " rounds=" << result.refinement_rounds
            << " area=" << result.polygon.planar_signed_area_m2
            << " estimated_delta=" << result.estimated_planar_area_error_m2
            << " allowed_delta=" << result.allowed_planar_area_delta_m2
            << '\n';
        return false;
    }

    if (result.refinement_rounds < 2U ||
        result.final_curve_geometric_tolerance_m >=
            options.initial.curve.geometric_tolerance_m ||
        result.final_horizon_arc_tolerance_m >=
            options.initial.horizon_arc_tolerance_m) {
        std::cerr << "thin sliver did not exercise refinement\n";
        return false;
    }
    if (!result.topology_stable ||
        !result.component_orientation_stable ||
        result.polygon.horizon_crossings != 2U ||
        result.polygon.rings.size() != 1U ||
        !(result.polygon.planar_signed_area_m2 > 0.0)) {
        std::cerr << "thin sliver did not preserve verified topology/orientation\n";
        return false;
    }
    if (!(result.estimated_planar_area_error_m2 <=
          result.allowed_planar_area_delta_m2)) {
        std::cerr << "thin sliver accepted without area convergence\n";
        return false;
    }

    return true;
}

bool test_invalid_verification_options_fail_at_boundary() {
    const auto ring = make_ring(
        {
            {radians(-10.0), radians(-10.0)},
            {radians(10.0), radians(-10.0)},
            {radians(10.0), radians(10.0)},
            {radians(-10.0), radians(10.0)},
        },
        aeris::geometry::RingInteriorSide::left
    );

    auto options = coarse_verified_options();
    options.max_refinement_rounds = 1U;
    const auto result =
        aeris::view::project_visible_wgs84_linear_polygon_ring_verified(
            ring,
            aeris::geo::Mat3{},
            options,
            aeris::geo::authalic_radius_m()
        );
    if (result.error != aeris::view::VerifiedGlobePolygonError::invalid_options) {
        std::cerr << "single-round verification options were not rejected\n";
        return false;
    }

    return true;
}

}  // namespace

int main() {
    if (!test_thin_horizon_sliver_refines() ||
        !test_invalid_verification_options_fail_at_boundary()) {
        return EXIT_FAILURE;
    }

    std::cout << "globe_polygon_verified: PASS\n";
    return EXIT_SUCCESS;
}
