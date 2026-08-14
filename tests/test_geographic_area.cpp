// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/geometry/geographic.hpp"

#include "aeris/geo/wgs84.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
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
        std::cerr.precision(17);
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

[[nodiscard]] double area_check_tolerance(
    const aeris::geometry::GeographicAreaResult& area,
    const double expected
) noexcept {
    // The production area path now reports both quadrature error and the
    // binary64 floor of q(phi). Keep the analytic test tied to that published
    // uncertainty instead of inventing a global relative tolerance. The small
    // arithmetic floor covers the final binary64 comparison itself.
    const double comparison_floor =
        32.0 * std::numeric_limits<double>::epsilon() *
        std::max(1.0, std::abs(expected));
    return std::max(2.0 * area.estimated_abs_error_m2, comparison_floor);
}

[[nodiscard]] double analytic_polar_cap_area(const double latitude_deg) {
    const long double a = static_cast<long double>(aeris::geo::Wgs84::semi_major_axis_m);
    const long double pi = static_cast<long double>(aeris::geo::kPi);
    const long double q_pole = static_cast<long double>(aeris::geo::authalic_q_pole());
    const long double q_latitude = static_cast<long double>(
        aeris::geo::authalic_q(radians(std::abs(latitude_deg)))
    );
    return static_cast<double>(pi * a * a * (q_pole - q_latitude));
}

[[nodiscard]] double analytic_surface_area() {
    const long double a = static_cast<long double>(aeris::geo::Wgs84::semi_major_axis_m);
    const long double pi = static_cast<long double>(aeris::geo::kPi);
    const long double q_pole = static_cast<long double>(aeris::geo::authalic_q_pole());
    return static_cast<double>(2.0L * pi * a * a * q_pole);
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
    expect_true(
        "canonicalization does not invent an interior side",
        ring.value.interior_side == aeris::geometry::RingInteriorSide::unspecified
    );
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
    expect_true("constant-latitude rectangle has bounded numerical estimate", area.estimated_abs_error_m2 < 100.0);

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

[[nodiscard]] aeris::geometry::LinearRingResult make_parallel_ring(const double latitude_deg) {
    const std::vector<aeris::geometry::GeodeticPoint> input{
        {radians(0.0), radians(latitude_deg)},
        {radians(90.0), radians(latitude_deg)},
        {radians(180.0), radians(latitude_deg)},
        {radians(-90.0), radians(latitude_deg)},
    };
    return aeris::geometry::canonicalize_wgs84_linear_ring(input);
}

void test_nonzero_winding_fails_closed_without_topology() {
    const auto ring = make_parallel_ring(80.0);
    expect_true("polar-style ring canonicalizes structurally", ring.ok());
    if (!ring.ok()) {
        return;
    }

    expect_true("polar-style ring exposes winding", ring.value.longitude_winding == 1);
    const auto area = aeris::geometry::signed_wgs84_linear_ring_area(ring.value);
    expect_true(
        "nonzero winding without interior topology fails closed",
        area.error == aeris::geometry::GeographicError::longitude_winding_unsupported
    );
}

void test_explicit_polar_interior_areas() {
    const double surface_area = analytic_surface_area();
    const double cap_area = analytic_polar_cap_area(80.0);

    auto north = make_parallel_ring(80.0);
    expect_true("north polar ring canonicalizes", north.ok());
    if (!north.ok()) {
        return;
    }
    north.value.interior_side = aeris::geometry::RingInteriorSide::left;
    const auto north_area = aeris::geometry::signed_wgs84_linear_ring_area(north.value);
    expect_true("north polar left-side area succeeds", north_area.ok());
    if (north_area.ok()) {
        expect_near(
            "north polar cap area",
            north_area.signed_area_m2,
            cap_area,
            area_check_tolerance(north_area, cap_area)
        );
        expect_true("north polar cap is positive left-side area", north_area.signed_area_m2 > 0.0);
        expect_true("north polar reported uncertainty stays small", north_area.estimated_abs_error_m2 < 100.0);

        auto north_complement = north.value;
        north_complement.interior_side = aeris::geometry::RingInteriorSide::right;
        const auto complement_area = aeris::geometry::signed_wgs84_linear_ring_area(north_complement);
        expect_true("north complement right-side area succeeds", complement_area.ok());
        if (complement_area.ok()) {
            const double expected_complement = -(surface_area - cap_area);
            expect_near(
                "large complement is represented rather than forced to minor region",
                complement_area.signed_area_m2,
                expected_complement,
                area_check_tolerance(complement_area, expected_complement)
            );
            expect_true(
                "explicit complement may exceed a hemisphere",
                std::abs(complement_area.signed_area_m2) > 0.5 * surface_area
            );
        }
    }

    auto south = make_parallel_ring(-80.0);
    expect_true("south polar ring canonicalizes", south.ok());
    if (!south.ok()) {
        return;
    }
    south.value.interior_side = aeris::geometry::RingInteriorSide::right;
    const auto south_area = aeris::geometry::signed_wgs84_linear_ring_area(south.value);
    expect_true("south polar right-side area succeeds", south_area.ok());
    if (south_area.ok()) {
        expect_near(
            "south polar cap area",
            south_area.signed_area_m2,
            -cap_area,
            area_check_tolerance(south_area, -cap_area)
        );
        expect_true("south polar cap preserves clockwise/right sign", south_area.signed_area_m2 < 0.0);
        expect_true("south polar reported uncertainty stays small", south_area.estimated_abs_error_m2 < 100.0);
    }
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
        expect_true("reported integration error remains small", area.estimated_abs_error_m2 < 100.0);
    }
}

}  // namespace

int main() {
    test_antimeridian_unwrap();
    test_ambiguous_half_turn_rejected();
    test_rectangle_area_and_orientation();
    test_nonzero_winding_fails_closed_without_topology();
    test_explicit_polar_interior_areas();
    test_slanted_edges_use_adaptive_integral();

    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return EXIT_FAILURE;
    }

    std::cout << "geographic_area: PASS\n";
    return EXIT_SUCCESS;
}
