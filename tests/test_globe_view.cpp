// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/view/globe.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>

namespace {

int failures = 0;

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

void test_identity_camera() {
    const aeris::geo::Mat3 identity{};
    constexpr double radius = 10.0;

    const auto center = aeris::view::orthographic_globe_point(0.0, 0.0, identity, radius);
    expect_true("globe center succeeds", center.ok());
    expect_true("globe center visible", center.value.visible);
    expect_near("globe center x", center.value.x, 0.0, 0.0);
    expect_near("globe center y", center.value.y, 0.0, 0.0);
    expect_near("globe center depth", center.value.depth, radius, 0.0);

    const auto limb = aeris::view::orthographic_globe_point(aeris::geo::kHalfPi, 0.0, identity, radius);
    expect_true("globe limb succeeds", limb.ok());
    expect_true("globe limb visible by closed hemisphere rule", limb.value.visible);
    expect_near("globe limb x", limb.value.x, radius, 2e-15 * radius);
    expect_near("globe limb depth", limb.value.depth, 0.0, 2e-15 * radius);

    const auto hidden = aeris::view::orthographic_globe_point(aeris::geo::kPi, 0.0, identity, radius);
    expect_true("hidden hemisphere succeeds", hidden.ok());
    expect_true("far hemisphere hidden", !hidden.value.visible);
    expect_near("far hemisphere depth", hidden.value.depth, -radius, 2e-15 * radius);
}

void test_camera_rotation() {
    constexpr double radius = 7.0;
    const auto camera = aeris::geo::rotation_z(-aeris::geo::kHalfPi);
    const auto point = aeris::view::orthographic_globe_point(
        aeris::geo::kHalfPi,
        0.0,
        camera,
        radius
    );

    expect_true("rotated globe point succeeds", point.ok());
    expect_true("rotated globe point visible", point.value.visible);
    expect_near("rotation moves longitude 90 to center x", point.value.x, 0.0, 2e-15 * radius);
    expect_near("rotation moves longitude 90 to center y", point.value.y, 0.0, 2e-15 * radius);
    expect_near("rotation moves longitude 90 to front", point.value.depth, radius, 2e-15 * radius);
}

void test_rotation_matrix_validation() {
    const auto proper = aeris::geo::multiply(
        aeris::geo::rotation_y(0.37),
        aeris::geo::rotation_z(-1.21)
    );
    expect_true("composed rotation accepted", aeris::geo::is_rotation_matrix(proper));

    aeris::geo::Mat3 scaled{};
    scaled.m[0] = 2.0;
    expect_true("scaled matrix rejected", !aeris::geo::is_rotation_matrix(scaled));

    const auto rejected = aeris::view::orthographic_globe_point(0.0, 0.0, scaled, 1.0);
    expect_true(
        "globe rejects non-rotation camera",
        rejected.error == aeris::geo::MathError::numerical_domain_error
    );
}

void test_invalid_inputs() {
    const aeris::geo::Mat3 identity{};
    const double nan = std::numeric_limits<double>::quiet_NaN();

    expect_true(
        "globe NaN rejected",
        aeris::view::orthographic_globe_point(nan, 0.0, identity).error ==
            aeris::geo::MathError::non_finite_input
    );
    expect_true(
        "globe latitude range enforced",
        aeris::view::orthographic_globe_point(0.0, aeris::geo::kHalfPi + 1e-6, identity).error ==
            aeris::geo::MathError::latitude_out_of_range
    );
    expect_true(
        "globe nonpositive radius rejected",
        aeris::view::orthographic_globe_point(0.0, 0.0, identity, 0.0).error ==
            aeris::geo::MathError::numerical_domain_error
    );
}

}  // namespace

int main() {
    test_identity_camera();
    test_camera_rotation();
    test_rotation_matrix_validation();
    test_invalid_inputs();

    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return EXIT_FAILURE;
    }

    std::cout << "globe_view: PASS\n";
    return EXIT_SUCCESS;
}
