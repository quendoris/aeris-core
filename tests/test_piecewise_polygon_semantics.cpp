// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/projection/ring.hpp"

#include "aeris/geo/wgs84.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

[[nodiscard]] double radians(const double degrees) {
    return degrees * aeris::geo::kPi / 180.0;
}

void expect_true(const std::string_view name, const bool condition) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL " << name << '\n';
    }
}

[[nodiscard]] aeris::geometry::LinearRing make_ring(
    const std::vector<aeris::geometry::GeodeticPoint>& points,
    const aeris::geometry::RingInteriorSide side
) {
    auto result = aeris::geometry::canonicalize_wgs84_linear_ring(points);
    expect_true("polygon semantic fixture canonicalizes", result.ok());
    if (!result.ok()) {
        return {};
    }
    result.value.interior_side = side;
    return result.value;
}

[[nodiscard]] aeris::projection::PiecewiseRingProjectionResult project(
    const aeris::geometry::LinearRing& ring,
    const aeris::projection::ProjectionAdapter& adapter
) {
    aeris::projection::RingProjectionOptions options{};
    options.adapter = &adapter;
    options.relative_area_tolerance = 1e-9;
    options.absolute_area_tolerance_m2 = 1.0;
    options.initial_geometric_tolerance_m = 8.0;
    options.initial_local_area_tolerance_m2 = 1024.0;
    options.max_refinement_rounds = 18U;
    options.max_projection_pieces = 32U;
    return aeris::projection::project_wgs84_linear_ring_piecewise_verified(
        ring,
        options
    );
}

void test_seam_split_exterior_and_hole(
    const aeris::projection::ProjectionAdapter& adapter
) {
    const auto exterior = make_ring(
        {
            {radians(160.0), radians(-40.0)},
            {radians(-160.0), radians(-40.0)},
            {radians(-160.0), radians(40.0)},
            {radians(160.0), radians(40.0)},
        },
        aeris::geometry::RingInteriorSide::left
    );

    const auto hole = make_ring(
        {
            {radians(175.0), radians(-10.0)},
            {radians(175.0), radians(10.0)},
            {radians(-175.0), radians(10.0)},
            {radians(-175.0), radians(-10.0)},
        },
        aeris::geometry::RingInteriorSide::right
    );

    const auto exterior_projected = project(exterior, adapter);
    const auto hole_projected = project(hole, adapter);

    expect_true("split exterior projects", exterior_projected.ok());
    expect_true("split hole projects", hole_projected.ok());
    if (!exterior_projected.ok() || !hole_projected.ok()) {
        std::cerr
            << "  adapter=" << adapter.descriptor().model_id
            << " exterior_error=" << static_cast<int>(exterior_projected.error)
            << " exterior_seam_error=" << static_cast<int>(exterior_projected.seam_error)
            << " exterior_piece_error=" << static_cast<int>(exterior_projected.piece_error)
            << " hole_error=" << static_cast<int>(hole_projected.error)
            << " hole_seam_error=" << static_cast<int>(hole_projected.seam_error)
            << " hole_piece_error=" << static_cast<int>(hole_projected.piece_error)
            << '\n';
        return;
    }

    expect_true("exterior is split into two pieces", exterior_projected.projected_pieces == 2U);
    expect_true("hole is split into two pieces", hole_projected.projected_pieces == 2U);
    expect_true("exterior preserves positive sign", exterior_projected.source_signed_area_m2 > 0.0 && exterior_projected.planar_signed_area_m2 > 0.0);
    expect_true("hole preserves negative sign", hole_projected.source_signed_area_m2 < 0.0 && hole_projected.planar_signed_area_m2 < 0.0);

    const double source_semantic_area =
        exterior_projected.source_signed_area_m2 +
        hole_projected.source_signed_area_m2;
    const double planar_semantic_area =
        exterior_projected.planar_signed_area_m2 +
        hole_projected.planar_signed_area_m2;
    const double semantic_error = std::abs(
        planar_semantic_area - source_semantic_area
    );
    const double combined_budget =
        exterior_projected.allowed_area_error_m2 +
        hole_projected.allowed_area_error_m2;

    expect_true("polygon-with-hole semantic area remains positive", source_semantic_area > 0.0);
    expect_true(
        "split exterior plus split hole preserves semantic area",
        std::isfinite(semantic_error) && semantic_error <= combined_budget
    );
}

}  // namespace

int main() {
    for (const auto* adapter : aeris::projection::builtin_projection_adapters()) {
        if (adapter == nullptr) {
            ++failures;
            continue;
        }
        test_seam_split_exterior_and_hole(*adapter);
    }

    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return EXIT_FAILURE;
    }

    std::cout << "piecewise_polygon_semantics: PASS\n";
    return EXIT_SUCCESS;
}
