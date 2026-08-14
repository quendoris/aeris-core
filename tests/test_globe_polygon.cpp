// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/view/globe_polygon.hpp"

#include "aeris/geo/wgs84.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iomanip>
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
        std::cerr << std::setprecision(17)
                  << "FAIL " << name
                  << ": actual=" << actual
                  << " expected=" << expected
                  << " tolerance=" << tolerance << '\n';
    }
}

void expect_ok(
    const std::string_view name,
    const aeris::view::GlobePolygonResult& result
) {
    if (result.ok()) {
        return;
    }
    ++failures;
    std::cerr << std::setprecision(17)
              << "FAIL " << name
              << ": error=" << static_cast<int>(result.error)
              << " geographic_error=" << static_cast<int>(result.geographic_error)
              << " curve_error=" << static_cast<int>(result.curve_error)
              << " sample_error=" << static_cast<int>(result.sample_error)
              << " source_m2=" << result.source_signed_area_m2
              << " planar_m2=" << result.planar_signed_area_m2
              << " disk_m2=" << result.visible_disk_area_m2
              << " rings=" << result.rings.size()
              << " crossings=" << result.horizon_crossings
              << " arc_segments=" << result.horizon_arc_segments
              << " vertices=" << result.projected_vertices
              << '\n';
}

[[nodiscard]] aeris::view::GlobePolygonOptions strict_options() {
    aeris::view::GlobePolygonOptions options{};
    options.curve.geometric_tolerance_m = 1e-4;
    options.curve.horizon_tolerance_m = 1e-10;
    options.curve.max_subdivision_depth = 32U;
    options.curve.max_root_iterations = 80U;
    options.curve.max_segments = 100'000U;
    options.horizon_arc_tolerance_m = 1e-4;
    options.max_horizon_arc_segments = 100'000U;
    options.max_output_rings = 64U;
    return options;
}

[[nodiscard]] aeris::geometry::LinearRing make_ring(
    const std::vector<aeris::geometry::GeodeticPoint>& points,
    const aeris::geometry::RingInteriorSide side
) {
    auto canonical = aeris::geometry::canonicalize_wgs84_linear_ring(points);
    expect_true("globe polygon fixture canonicalizes", canonical.ok());
    if (!canonical.ok()) {
        return {};
    }
    canonical.value.interior_side = side;
    return canonical.value;
}

[[nodiscard]] double signed_area_sum(
    const aeris::view::GlobePolygonResult& result
) {
    double sum = 0.0;
    for (const auto& ring : result.rings) {
        sum += aeris::geometry::signed_planar_area(ring);
    }
    return sum;
}

void check_implicit_closed_rings(
    const std::string_view name,
    const aeris::view::GlobePolygonResult& result
) {
    for (const auto& ring : result.rings) {
        expect_true(name, ring.size() >= 3U);
        if (ring.size() >= 2U) {
            expect_true(
                name,
                std::hypot(
                    ring.front().x - ring.back().x,
                    ring.front().y - ring.back().y
                ) > 1e-12
            );
        }
    }
}

void test_fully_visible_minor_region() {
    const auto ring = make_ring(
        {
            {radians(-30.0), radians(-20.0)},
            {radians(30.0), radians(-20.0)},
            {radians(30.0), radians(20.0)},
            {radians(-30.0), radians(20.0)},
        },
        aeris::geometry::RingInteriorSide::left
    );

    const auto result = aeris::view::project_visible_wgs84_linear_polygon_ring(
        ring,
        aeris::geo::Mat3{},
        strict_options(),
        10.0
    );

    expect_ok("fully visible minor polygon succeeds", result);
    if (!result.ok()) {
        return;
    }
    expect_true("fully visible minor polygon is one ring", result.rings.size() == 1U);
    expect_true("fully visible minor polygon needs no limb arc", result.horizon_arc_segments == 0U);
    expect_true("fully visible minor polygon has no crossing", result.horizon_crossings == 0U);
    expect_true("fully visible minor polygon keeps positive orientation", result.planar_signed_area_m2 > 0.0);
    expect_near(
        "reported visible area matches ring sum",
        result.planar_signed_area_m2,
        signed_area_sum(result),
        1e-12
    );
    check_implicit_closed_rings("fully visible minor ring is implicit-closed", result);
}

void test_fully_hidden_minor_region_is_empty() {
    const auto ring = make_ring(
        {
            {radians(120.0), radians(-20.0)},
            {radians(160.0), radians(-20.0)},
            {radians(160.0), radians(20.0)},
            {radians(120.0), radians(20.0)},
        },
        aeris::geometry::RingInteriorSide::left
    );

    const auto result = aeris::view::project_visible_wgs84_linear_polygon_ring(
        ring,
        aeris::geo::Mat3{},
        strict_options(),
        10.0
    );

    expect_ok("fully hidden minor polygon succeeds", result);
    if (!result.ok()) {
        return;
    }
    expect_true("fully hidden minor polygon emits no ring", result.rings.empty());
    expect_near("fully hidden minor projected area is zero", result.planar_signed_area_m2, 0.0, 1e-12);
}

void test_fully_hidden_major_complement_is_full_disk() {
    constexpr double radius = 10.0;
    const auto ring = make_ring(
        {
            {radians(120.0), radians(-20.0)},
            {radians(160.0), radians(-20.0)},
            {radians(160.0), radians(20.0)},
            {radians(120.0), radians(20.0)},
        },
        aeris::geometry::RingInteriorSide::right
    );

    const auto result = aeris::view::project_visible_wgs84_linear_polygon_ring(
        ring,
        aeris::geo::Mat3{},
        strict_options(),
        radius
    );

    expect_ok("hidden major complement succeeds", result);
    if (!result.ok()) {
        return;
    }
    expect_true("hidden major complement emits limb ring", result.rings.size() == 1U);
    expect_true("hidden major complement uses horizon arc segments", result.horizon_arc_segments >= 4U);
    expect_true("hidden major complement preserves right negative sign", result.planar_signed_area_m2 < 0.0);

    const double segments = static_cast<double>(result.horizon_arc_segments);
    const double expected_finite_disk_area =
        0.5 * segments * radius * radius *
        std::sin(2.0 * aeris::geo::kPi / segments);
    const double area_roundoff =
        256.0 * std::numeric_limits<double>::epsilon() * segments *
        std::max(1.0, expected_finite_disk_area);

    expect_near(
        "hidden major complement matches finite limb polygon",
        result.planar_signed_area_m2,
        -expected_finite_disk_area,
        area_roundoff
    );
    expect_true(
        "finite full-disk approximation remains inside exact disk",
        std::abs(result.planar_signed_area_m2) < result.visible_disk_area_m2
    );
}

void test_fully_visible_major_complement_is_disk_minus_boundary() {
    const auto ring = make_ring(
        {
            {radians(-30.0), radians(-20.0)},
            {radians(30.0), radians(-20.0)},
            {radians(30.0), radians(20.0)},
            {radians(-30.0), radians(20.0)},
        },
        aeris::geometry::RingInteriorSide::right
    );

    const auto result = aeris::view::project_visible_wgs84_linear_polygon_ring(
        ring,
        aeris::geo::Mat3{},
        strict_options(),
        10.0
    );

    expect_ok("visible major complement succeeds", result);
    if (!result.ok()) {
        return;
    }
    expect_true("visible major complement emits outer plus hole", result.rings.size() == 2U);
    expect_true("visible major complement has negative aggregate sign", result.planar_signed_area_m2 < 0.0);
    expect_true(
        "visible major complement is finite disk minus exclusion",
        std::abs(result.planar_signed_area_m2) < result.visible_disk_area_m2
    );
    check_implicit_closed_rings("visible major complement rings are implicit-closed", result);
}

void test_partial_left_region_closes_on_limb() {
    const auto ring = make_ring(
        {
            {radians(60.0), radians(-20.0)},
            {radians(120.0), radians(-20.0)},
            {radians(120.0), radians(20.0)},
            {radians(60.0), radians(20.0)},
        },
        aeris::geometry::RingInteriorSide::left
    );

    const auto result = aeris::view::project_visible_wgs84_linear_polygon_ring(
        ring,
        aeris::geo::Mat3{},
        strict_options(),
        10.0
    );

    expect_ok("partial left polygon succeeds", result);
    if (!result.ok()) {
        return;
    }
    expect_true("partial left polygon has two horizon crossings", result.horizon_crossings == 2U);
    expect_true("partial left polygon closes as one visible region", result.rings.size() == 1U);
    expect_true("partial left polygon adds limb arc", result.horizon_arc_segments > 0U);
    expect_true("partial left polygon keeps positive sign", result.planar_signed_area_m2 > 0.0);
    check_implicit_closed_rings("partial left result is implicit-closed", result);
}

void test_partial_right_region_reverses_limb_orientation() {
    std::vector<aeris::geometry::GeodeticPoint> points{
        {radians(60.0), radians(-20.0)},
        {radians(120.0), radians(-20.0)},
        {radians(120.0), radians(20.0)},
        {radians(60.0), radians(20.0)},
    };
    std::reverse(points.begin(), points.end());
    const auto ring = make_ring(points, aeris::geometry::RingInteriorSide::right);

    const auto result = aeris::view::project_visible_wgs84_linear_polygon_ring(
        ring,
        aeris::geo::Mat3{},
        strict_options(),
        10.0
    );

    expect_ok("partial right polygon succeeds", result);
    if (!result.ok()) {
        return;
    }
    expect_true("partial right polygon closes as one region", result.rings.size() == 1U);
    expect_true("partial right polygon keeps negative sign", result.planar_signed_area_m2 < 0.0);
    expect_true("partial right polygon uses limb arc", result.horizon_arc_segments > 0U);
}

void test_same_boundary_can_select_long_complement_arc() {
    const std::vector<aeris::geometry::GeodeticPoint> points{
        {radians(60.0), radians(-20.0)},
        {radians(120.0), radians(-20.0)},
        {radians(120.0), radians(20.0)},
        {radians(60.0), radians(20.0)},
    };
    const auto left_ring = make_ring(points, aeris::geometry::RingInteriorSide::left);
    const auto right_ring = make_ring(points, aeris::geometry::RingInteriorSide::right);

    const auto left = aeris::view::project_visible_wgs84_linear_polygon_ring(
        left_ring,
        aeris::geo::Mat3{},
        strict_options(),
        10.0
    );
    const auto right = aeris::view::project_visible_wgs84_linear_polygon_ring(
        right_ring,
        aeris::geo::Mat3{},
        strict_options(),
        10.0
    );

    expect_ok("same-boundary left region succeeds", left);
    expect_ok("same-boundary right complement succeeds", right);
    if (!left.ok() || !right.ok()) {
        return;
    }
    expect_true("same boundary left region is positive", left.planar_signed_area_m2 > 0.0);
    expect_true("same boundary right complement is negative", right.planar_signed_area_m2 < 0.0);
    expect_true(
        "right side selects long complement rather than shortest limb arc",
        std::abs(right.planar_signed_area_m2) > std::abs(left.planar_signed_area_m2)
    );
}

void test_four_crossings_can_form_two_visible_components() {
    const auto ring = make_ring(
        {
            {radians(120.0), radians(50.0)},
            {radians(60.0), radians(50.0)},
            {radians(60.0), radians(20.0)},
            {radians(100.0), radians(20.0)},
            {radians(100.0), radians(-20.0)},
            {radians(60.0), radians(-20.0)},
            {radians(60.0), radians(-50.0)},
            {radians(120.0), radians(-50.0)},
        },
        aeris::geometry::RingInteriorSide::left
    );

    const auto result = aeris::view::project_visible_wgs84_linear_polygon_ring(
        ring,
        aeris::geo::Mat3{},
        strict_options(),
        10.0
    );

    expect_ok("four-crossing visible components succeed", result);
    if (!result.ok()) {
        return;
    }
    expect_true("four-crossing fixture exposes four crossings", result.horizon_crossings == 4U);
    expect_true("four-crossing fixture forms two visible components", result.rings.size() == 2U);
    expect_true("two-component result stays positive", result.planar_signed_area_m2 > 0.0);
}

void test_missing_interior_side_fails_closed() {
    auto ring = make_ring(
        {
            {radians(60.0), radians(-20.0)},
            {radians(120.0), radians(-20.0)},
            {radians(120.0), radians(20.0)},
            {radians(60.0), radians(20.0)},
        },
        aeris::geometry::RingInteriorSide::left
    );
    ring.interior_side = aeris::geometry::RingInteriorSide::unspecified;

    const auto result = aeris::view::project_visible_wgs84_linear_polygon_ring(
        ring,
        aeris::geo::Mat3{},
        strict_options(),
        10.0
    );
    expect_true(
        "missing globe polygon interior side fails closed",
        result.error == aeris::view::GlobePolygonError::missing_interior_side
    );
}

void test_invalid_arc_options_fail_at_boundary() {
    const auto ring = make_ring(
        {
            {radians(-30.0), radians(-20.0)},
            {radians(30.0), radians(-20.0)},
            {radians(30.0), radians(20.0)},
            {radians(-30.0), radians(20.0)},
        },
        aeris::geometry::RingInteriorSide::left
    );
    auto options = strict_options();
    options.horizon_arc_tolerance_m = 0.0;

    const auto result = aeris::view::project_visible_wgs84_linear_polygon_ring(
        ring,
        aeris::geo::Mat3{},
        options,
        10.0
    );
    expect_true(
        "zero horizon arc tolerance rejected",
        result.error == aeris::view::GlobePolygonError::invalid_options
    );
}

}  // namespace

int main() {
    test_fully_visible_minor_region();
    test_fully_hidden_minor_region_is_empty();
    test_fully_hidden_major_complement_is_full_disk();
    test_fully_visible_major_complement_is_disk_minus_boundary();
    test_partial_left_region_closes_on_limb();
    test_partial_right_region_reverses_limb_orientation();
    test_same_boundary_can_select_long_complement_arc();
    test_four_crossings_can_form_two_visible_components();
    test_missing_interior_side_fails_closed();
    test_invalid_arc_options_fail_at_boundary();

    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return EXIT_FAILURE;
    }

    std::cout << "globe_polygon: PASS\n";
    return EXIT_SUCCESS;
}
