// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/geometry/geographic.hpp"
#include "aeris/projection/ring.hpp"

#include "aeris/geo/wgs84.hpp"

#include <algorithm>
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
    const std::vector<aeris::geometry::GeodeticPoint>& points
) {
    const auto result = aeris::geometry::canonicalize_wgs84_linear_ring(points);
    expect_true("verified projection input ring canonicalizes", result.ok());
    return result.ok() ? result.value : aeris::geometry::LinearRing{};
}

void check_verified_projection(
    const aeris::geometry::LinearRing& ring,
    const aeris::projection::EqualAreaPrimitive primitive,
    const double central_meridian
) {
    aeris::projection::RingProjectionOptions options{};
    options.primitive = primitive;
    options.central_meridian_rad = central_meridian;
    options.relative_area_tolerance = 1e-9;
    options.absolute_area_tolerance_m2 = 1.0;
    options.initial_geometric_tolerance_m = 8.0;
    options.initial_local_area_tolerance_m2 = 1024.0;
    options.max_refinement_rounds = 10U;

    const auto result = aeris::projection::project_wgs84_linear_ring_verified(
        ring,
        options
    );

    expect_true("verified ring projection succeeds", result.ok());
    if (!result.ok()) {
        std::cerr << "  ring projection error=" << static_cast<int>(result.error)
                  << " geographic_error=" << static_cast<int>(result.geographic_error)
                  << " subdivision_error=" << static_cast<int>(result.subdivision_error)
                  << " sample_error=" << static_cast<int>(result.sample_error)
                  << " rounds=" << result.refinement_rounds
                  << " area_error=" << result.absolute_area_error_m2
                  << " allowed=" << result.allowed_area_error_m2 << '\n';
        return;
    }

    expect_true("verified result has finite source area", std::isfinite(result.source_signed_area_m2));
    expect_true("verified result has finite planar area", std::isfinite(result.planar_signed_area_m2));
    expect_true("verified result contains a polygon", result.projected_vertices >= 4U);
    expect_true(
        "verified result satisfies its published area budget",
        result.absolute_area_error_m2 <= result.allowed_area_error_m2
    );
    expect_true("nontrivial polygon required refinement", result.refinement_rounds > 1U);
}

void test_oblique_ring_budget() {
    const auto ring = make_ring({
        {radians(-35.0), radians(-20.0)},
        {radians(25.0), radians(-12.0)},
        {radians(40.0), radians(28.0)},
        {radians(-15.0), radians(42.0)},
    });

    check_verified_projection(
        ring,
        aeris::projection::EqualAreaPrimitive::sinusoidal,
        0.0
    );
    check_verified_projection(
        ring,
        aeris::projection::EqualAreaPrimitive::mollweide,
        0.0
    );
}

void test_antimeridian_ring_budget() {
    const auto ring = make_ring({
        {radians(170.0), radians(-20.0)},
        {radians(-170.0), radians(-20.0)},
        {radians(-170.0), radians(20.0)},
        {radians(170.0), radians(20.0)},
    });

    check_verified_projection(
        ring,
        aeris::projection::EqualAreaPrimitive::sinusoidal,
        aeris::geo::kPi
    );
    check_verified_projection(
        ring,
        aeris::projection::EqualAreaPrimitive::mollweide,
        aeris::geo::kPi
    );
}

void test_impossible_budget_fails_closed() {
    const auto ring = make_ring({
        {radians(-35.0), radians(-20.0)},
        {radians(25.0), radians(-12.0)},
        {radians(40.0), radians(28.0)},
        {radians(-15.0), radians(42.0)},
    });

    aeris::projection::RingProjectionOptions options{};
    options.primitive = aeris::projection::EqualAreaPrimitive::mollweide;
    options.relative_area_tolerance = 0.0;
    options.absolute_area_tolerance_m2 = 0.001;
    options.initial_geometric_tolerance_m = 1000.0;
    options.initial_local_area_tolerance_m2 = 1e8;
    options.max_refinement_rounds = 1U;

    const auto result = aeris::projection::project_wgs84_linear_ring_verified(
        ring,
        options
    );

    expect_true(
        "unmet maximum-quality area budget fails closed",
        result.error == aeris::projection::RingProjectionError::area_budget_unmet
    );
    expect_true(
        "failed budget still reports measured error",
        std::isfinite(result.absolute_area_error_m2) && result.absolute_area_error_m2 > 0.0
    );
}

void test_orientation_is_preserved() {
    std::vector<aeris::geometry::GeodeticPoint> points{
        {radians(-10.0), radians(-10.0)},
        {radians(20.0), radians(-10.0)},
        {radians(20.0), radians(20.0)},
        {radians(-10.0), radians(20.0)},
    };
    std::reverse(points.begin(), points.end());
    const auto ring = make_ring(points);

    aeris::projection::RingProjectionOptions options{};
    options.relative_area_tolerance = 1e-9;
    const auto result = aeris::projection::project_wgs84_linear_ring_verified(ring, options);

    expect_true("reversed verified ring succeeds", result.ok());
    if (result.ok()) {
        expect_true("reversed source area remains negative", result.source_signed_area_m2 < 0.0);
        expect_true("reversed planar area remains negative", result.planar_signed_area_m2 < 0.0);
    }
}

}  // namespace

int main() {
    test_oblique_ring_budget();
    test_antimeridian_ring_budget();
    test_impossible_budget_fails_closed();
    test_orientation_is_preserved();

    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return EXIT_FAILURE;
    }

    std::cout << "ring_projection_budget: PASS\n";
    return EXIT_SUCCESS;
}
