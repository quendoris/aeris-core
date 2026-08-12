// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/geometry/planar.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

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

void expect_true(const std::string_view name, const bool condition) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL " << name << '\n';
    }
}

void test_orientation_and_translation() {
    using aeris::geometry::PlanarPoint;

    const std::vector<PlanarPoint> square{
        {1.0e12, -2.0e12},
        {1.0e12 + 3.0, -2.0e12},
        {1.0e12 + 3.0, -2.0e12 + 4.0},
        {1.0e12, -2.0e12 + 4.0},
    };
    expect_near("translated square area", aeris::geometry::signed_planar_area(square), 12.0, 0.0);

    const std::vector<PlanarPoint> reversed(square.rbegin(), square.rend());
    expect_near("reversed square area", aeris::geometry::signed_planar_area(reversed), -12.0, 0.0);
}

void test_degenerate_inputs() {
    using aeris::geometry::PlanarPoint;

    const std::vector<PlanarPoint> line{{0.0, 0.0}, {1.0, 1.0}};
    expect_near("two points have zero area", aeris::geometry::signed_planar_area(line), 0.0, 0.0);
    expect_near("null polygon has zero area", aeris::geometry::signed_planar_area(nullptr, 100U), 0.0, 0.0);

    const std::vector<PlanarPoint> invalid{
        {0.0, 0.0},
        {1.0, 0.0},
        {std::numeric_limits<double>::infinity(), 1.0},
    };
    expect_true("non-finite polygon produces NaN", std::isnan(aeris::geometry::signed_planar_area(invalid)));
}

}  // namespace

int main() {
    test_orientation_and_translation();
    test_degenerate_inputs();

    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return EXIT_FAILURE;
    }

    std::cout << "planar_geometry: PASS\n";
    return EXIT_SUCCESS;
}
