// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/geometry/geographic.hpp"

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

void expect_near(
    const std::string_view name,
    const double actual,
    const double expected,
    const double tolerance
) {
    if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance) {
        ++failures;
        std::cerr << "FAIL " << name << ": actual=" << actual
                  << " expected=" << expected << " tolerance=" << tolerance << '\n';
    }
}

void expect_relative(
    const std::string_view name,
    const double actual,
    const double expected,
    const double relative_tolerance
) {
    const double scale = std::max(1.0, std::abs(expected));
    expect_near(name, actual, expected, relative_tolerance * scale);
}

void test_antimeridian_unwrap() {
    const std::vector<aeris::geometry::GeodeticPoint> input{
        {radians(170.0), radians(-10.0)},
        {radians(-170.0), radians(-10.0)},
        {radians(-170.0), radians(10.0)},
        {radians(170.0), radians(10.0)},
        {radians(170.0), radians(-10.0)},
    };

    const auto ring = aeris::geometry::canonicalize_wgs84_linear_ring(input);
    expect_true("antimeridian ring canonicalizes", ring.ok());
    if (!ring.ok()) {
        return;
    }

    expect_true("duplicate terminal vertex removed", ring.value.vertices.size() == 4U);
    expect_near("first longitude is 170 degrees", ring.value.vertices[0].longitude_rad, radians(170.0), 2e-15);
    expect_near("second longitude unwraps to 190 degrees", ring.value.vertices[1].longitude_rad, radians(190.0), 2e-15);
    expect_near("third longitude remains 190 degrees", ring.value.vertices[2].longitude_rad, radians(190.0), 2e-15);
    expect_near("fourth longitude returns to 170 degrees", ring.value.vertices[3].longitude_rad, radians(170.0), 2e-15);
    expect_true("ordinary antimeridian ring has zero winding", ring.value.longitude_winding == 0);
}

void test_ambiguous_half_turn_rejected() {
    const std::vector<aeris::geometry::GeodeticPoint> input{
        {radians(0.0), radians(0.0)},
        {radians(180.0), radians(10.0)},
        {radians(20.0), radians(20.0)},
    };

    const auto ring = aeris::geometry::canonicalize_wgs84_linear_ring(input);
    expect_true(
        "exact half-turn edge rejected",
        ring.error == aeris::geometry::GeographicError::ambiguous_half_turn
    );
}

void test_rectangle_area_and_orientation() {
    const double west = radians(-10.0);
    const double east = radians(30.0);
    const double south = radians(-20.0);
    const double north = radians(25.0);

    std::vector<aeris::geometry::GeodeticPoint> input{
        {west, south},
        {east, south},
        {east, north},
        {west, north},
    };

    const auto ring = aeris::geometry::canonicalize_wgs84_linear_ring(input);
    expect_true("rectangle canonicalizes", ring.ok());
    if (!ring.ok()) {
        return;
    }

    const auto area = aeris::geometry::signed_wgs84_linear_ring_area(ring.value);
    expect_true("rectangle area succeeds", area.ok());
    if (!area.ok()) {
        return;
    }

    const double a = aeris::geo::Wgs84::semi_major_axis_m;
    const double expected =
        0.5 * a * a * (east - west) *
        (aeris::geo::authalic_q(north) - aeris::geo::authalic_q(south));

    expect_relative("rectangle WGS84 area", area.signed_area_m2, expected, 2e-15);
    expect_true("rectangle orientation is positive", area.signed_area_m2 > 0.0);
    expect_true("constant-latitude rectangle has negligible integration estimate", area.estimated_abs_error_m2 < 1e-6);

    std::reverse(input.begin(), input.end());
    const auto reversed_ring = aeris::geometry::canonicalize_wgs84_linear_ring(input);
    expect_true("reversed rectangle canonicalizes", reversed_ring.ok());
    if (!reversed_ring.ok()) {
        return;
    }

    const auto reversed_area = aeris::geometry::signed_wgs84_linear_ring_area(reversed_ring.value);
    expect_true("reversed rectangle area succeeds", reversed_area.ok());
    if (reversed_area.ok()) {
        expect_relative("ring reversal flips signed area", reversed_area.signed_area_m2, -expected, 2e-15);
    }
}

void test_nonzero_winding_fails_closed() {
    const std::vector<aeris::geometry::GeodeticPoint> input{
        {radians(0.0), radians(80.0)},
        {radians(90.0), radians(80.0)},
        {radians(180.0), radians(80.0)},
        {radians(-90.0), radians(80.0)},
    };

    const auto ring = aeris::geometry::canonicalize_wgs84_linear_ring(input);
    expect_true("polar-style ring canonicalizes structurally", ring.ok());
    if (!ring.ok()) {
        return;
    }

    expect_true("polar-style ring exposes winding", ring.value.longitude_winding == 1);
    const auto area = aeris::geometry::signed_wgs84_linear_ring_area(ring.value);
    expect_true(
        "nonzero winding area fails closed",
        area.error == aeris::geometry::GeographicError::longitude_winding_unsupported
    );
}

void test_slanted_edges_use_adaptive_integral() {
    const std::vector<aeris::geometry::GeodeticPoint> input{
        {radians(-35.0), radians(-20.0)},
        {radians(25.0), radians(-12.0)},
        {radians(40.0), radians(28.0)},
        {radians(-15.0), radians(42.0)},
    };

    const auto ring = aeris::geometry::canonicalize_wgs84_linear_ring(input);
    expect_true("slanted ring canonicalizes", ring.ok());
    if (!ring.ok()) {
        return;
    }

    const auto area = aeris::geometry::signed_wgs84_linear_ring_area(ring.value);
    expect_true("slanted ring area succeeds", area.ok());
    if (area.ok()) {
        expect_true("slanted ring has positive area", area.signed_area_m2 > 0.0);
        expect_true("adaptive integral actually subdivides", area.deepest_integration_level > 0U);
        expect_true("reported integration error is finite", std::isfinite(area.estimated_abs_error_m2));
        expect_true("reported integration error remains small", area.estimated_abs_error_m2 < 10.0);
    }
}

}  // namespace

int main() {
    test_antimeridian_unwrap();
    test_ambiguous_half_turn_rejected();
    test_rectangle_area_and_orientation();
    test_nonzero_winding_fails_closed();
    test_slanted_edges_use_adaptive_integral();

    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return EXIT_FAILURE;
    }

    std::cout << "geographic_area: PASS\n";
    return EXIT_SUCCESS;
}
