// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/view/globe_curve.hpp"

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

void expect_near(
    const std::string_view name,
    const double actual,
    const double expected,
    const double tolerance
) {
    if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance) {
        ++failures;
        std::cerr << "FAIL " << name
                  << ": actual=" << actual
                  << " expected=" << expected
                  << " tolerance=" << tolerance << '\n';
    }
}

[[nodiscard]] aeris::view::GlobeCurveOptions strict_options() {
    aeris::view::GlobeCurveOptions options{};
    options.geometric_tolerance_m = 1e-4;
    options.horizon_tolerance_m = 1e-10;
    options.max_subdivision_depth = 32U;
    options.max_root_iterations = 80U;
    options.max_segments = 100'000U;
    return options;
}

[[nodiscard]] aeris::geometry::LinearRing make_ring(
    const std::vector<aeris::geometry::GeodeticPoint>& points
) {
    const auto canonical = aeris::geometry::canonicalize_wgs84_linear_ring(points);
    expect_true("globe curve ring canonicalizes", canonical.ok());
    return canonical.ok() ? canonical.value : aeris::geometry::LinearRing{};
}

void test_fully_visible_edge() {
    constexpr double radius = 10.0;
    const auto result = aeris::view::project_visible_wgs84_linear_edge(
        {radians(-30.0), 0.0},
        {radians(30.0), 0.0},
        aeris::geo::Mat3{},
        strict_options(),
        radius
    );

    expect_true("fully visible edge succeeds", result.ok());
    if (!result.ok()) {
        return;
    }
    expect_true("fully visible edge is one part", result.visible_parts.size() == 1U);
    expect_true("fully visible edge has no horizon crossing", result.horizon_crossings == 0U);
    expect_true("fully visible edge is adaptively sampled", result.projected_vertices > 2U);
    if (!result.visible_parts.empty()) {
        expect_near("visible edge starts at expected x", result.visible_parts[0].front().x, -5.0, 1e-12);
        expect_near("visible edge ends at expected x", result.visible_parts[0].back().x, 5.0, 1e-12);
    }
}

void test_visible_to_hidden_horizon_crossing() {
    constexpr double radius = 10.0;
    const auto result = aeris::view::project_visible_wgs84_linear_edge(
        {radians(60.0), 0.0},
        {radians(120.0), 0.0},
        aeris::geo::Mat3{},
        strict_options(),
        radius
    );

    expect_true("visible-hidden edge succeeds", result.ok());
    if (!result.ok()) {
        return;
    }
    expect_true("visible-hidden edge yields one visible part", result.visible_parts.size() == 1U);
    expect_true("visible-hidden edge has one crossing", result.horizon_crossings == 1U);
    if (result.visible_parts.size() == 1U) {
        const auto horizon = result.visible_parts[0].back();
        expect_near("east limb horizon x", horizon.x, radius, 1e-9);
        expect_near("east limb horizon y", horizon.y, 0.0, 1e-12);
        expect_near("east limb point lies on circle", std::hypot(horizon.x, horizon.y), radius, 1e-9);
    }
}

void test_hidden_to_visible_horizon_crossing() {
    constexpr double radius = 10.0;
    const auto result = aeris::view::project_visible_wgs84_linear_edge(
        {radians(120.0), 0.0},
        {radians(60.0), 0.0},
        aeris::geo::Mat3{},
        strict_options(),
        radius
    );

    expect_true("hidden-visible edge succeeds", result.ok());
    if (!result.ok()) {
        return;
    }
    expect_true("hidden-visible edge yields one visible part", result.visible_parts.size() == 1U);
    expect_true("hidden-visible edge has one crossing", result.horizon_crossings == 1U);
    if (result.visible_parts.size() == 1U) {
        const auto horizon = result.visible_parts[0].front();
        expect_near("reverse crossing starts on limb", std::hypot(horizon.x, horizon.y), radius, 1e-9);
    }
}

void test_partially_visible_ring_has_open_fragment() {
    constexpr double radius = 10.0;
    const auto ring = make_ring({
        {radians(60.0), radians(-20.0)},
        {radians(120.0), radians(-20.0)},
        {radians(120.0), radians(20.0)},
        {radians(60.0), radians(20.0)},
    });
    if (ring.vertices.empty()) {
        return;
    }

    const auto result = aeris::view::project_visible_wgs84_linear_ring(
        ring,
        aeris::geo::Mat3{},
        strict_options(),
        radius
    );

    expect_true("partially visible ring succeeds", result.ok());
    if (!result.ok()) {
        return;
    }
    expect_true("partially visible ring has two horizon crossings", result.horizon_crossings == 2U);
    expect_true("partially visible ring becomes one open fragment", result.visible_parts.size() == 1U);
    if (result.visible_parts.size() == 1U) {
        const auto& part = result.visible_parts[0];
        expect_true("partial ring fragment has multiple vertices", part.size() > 2U);
        expect_near("partial ring start is on limb", std::hypot(part.front().x, part.front().y), radius, 1e-8);
        expect_near("partial ring end is on limb", std::hypot(part.back().x, part.back().y), radius, 1e-8);
        expect_true(
            "partial ring is not artificially closed across hidden hemisphere",
            std::hypot(
                part.front().x - part.back().x,
                part.front().y - part.back().y
            ) > 1e-6
        );
    }
}

void test_fully_visible_ring_remains_closed() {
    constexpr double radius = 10.0;
    const auto ring = make_ring({
        {radians(-30.0), radians(-20.0)},
        {radians(30.0), radians(-20.0)},
        {radians(30.0), radians(20.0)},
        {radians(-30.0), radians(20.0)},
    });
    if (ring.vertices.empty()) {
        return;
    }

    const auto result = aeris::view::project_visible_wgs84_linear_ring(
        ring,
        aeris::geo::Mat3{},
        strict_options(),
        radius
    );

    expect_true("fully visible ring succeeds", result.ok());
    if (!result.ok()) {
        return;
    }
    expect_true("fully visible ring has no crossings", result.horizon_crossings == 0U);
    expect_true("fully visible ring is one polyline", result.visible_parts.size() == 1U);
    if (result.visible_parts.size() == 1U) {
        const auto& part = result.visible_parts[0];
        expect_true("fully visible ring contains samples", part.size() > 4U);
        expect_near("closed ring x closure", part.front().x, part.back().x, 1e-12);
        expect_near("closed ring y closure", part.front().y, part.back().y, 1e-12);
    }
}

void test_rotated_camera_moves_horizon_contract() {
    constexpr double radius = 10.0;
    const auto camera = aeris::geo::rotation_z(-aeris::geo::kHalfPi);
    const auto result = aeris::view::project_visible_wgs84_linear_edge(
        {radians(150.0), 0.0},
        {radians(210.0), 0.0},
        camera,
        strict_options(),
        radius
    );

    expect_true("rotated horizon edge succeeds", result.ok());
    if (result.ok()) {
        expect_true("rotated horizon edge has one crossing", result.horizon_crossings == 1U);
        expect_true("rotated horizon edge exposes visible fragment", result.visible_parts.size() == 1U);
    }
}

void test_resource_limit_fails_closed() {
    auto options = strict_options();
    options.geometric_tolerance_m = 1e-10;
    options.max_segments = 1U;

    const auto result = aeris::view::project_visible_wgs84_linear_edge(
        {radians(-60.0), 0.0},
        {radians(60.0), 0.0},
        aeris::geo::Mat3{},
        options,
        10.0
    );

    expect_true(
        "globe curve resource ceiling fails closed",
        result.error == aeris::view::GlobeCurveError::limit_exceeded
    );
}

void test_invalid_options_fail_at_boundary() {
    auto options = strict_options();
    options.horizon_tolerance_m = 0.0;
    const auto invalid_horizon = aeris::view::project_visible_wgs84_linear_edge(
        {0.0, 0.0},
        {radians(10.0), 0.0},
        aeris::geo::Mat3{},
        options,
        10.0
    );
    expect_true(
        "zero horizon tolerance rejected",
        invalid_horizon.error == aeris::view::GlobeCurveError::invalid_options
    );

    options = strict_options();
    options.max_root_iterations = 0U;
    const auto invalid_iterations = aeris::view::project_visible_wgs84_linear_edge(
        {0.0, 0.0},
        {radians(10.0), 0.0},
        aeris::geo::Mat3{},
        options,
        10.0
    );
    expect_true(
        "zero horizon root iterations rejected",
        invalid_iterations.error == aeris::view::GlobeCurveError::invalid_options
    );
}

}  // namespace

int main() {
    test_fully_visible_edge();
    test_visible_to_hidden_horizon_crossing();
    test_hidden_to_visible_horizon_crossing();
    test_partially_visible_ring_has_open_fragment();
    test_fully_visible_ring_remains_closed();
    test_rotated_camera_moves_horizon_contract();
    test_resource_limit_fails_closed();
    test_invalid_options_fail_at_boundary();

    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return EXIT_FAILURE;
    }

    std::cout << "globe_curve: PASS\n";
    return EXIT_SUCCESS;
}
