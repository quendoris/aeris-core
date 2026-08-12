// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/projection/primitives.hpp"

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

void test_sinusoidal_origin_and_round_trip() {
    const double radius = aeris::geo::authalic_radius_m();
    const auto origin = aeris::projection::sinusoidal_forward(0.0, 0.0, radius);
    expect_true("sinusoidal origin succeeds", origin.ok());
    expect_near("sinusoidal origin x", origin.value.x, 0.0, 0.0);
    expect_near("sinusoidal origin y", origin.value.y, 0.0, 0.0);

    for (int latitude_deg = -80; latitude_deg <= 80; latitude_deg += 10) {
        for (int longitude_deg = -170; longitude_deg <= 170; longitude_deg += 17) {
            const double beta = static_cast<double>(latitude_deg) * aeris::geo::kPi / 180.0;
            const double lambda = static_cast<double>(longitude_deg) * aeris::geo::kPi / 180.0;
            const auto projected = aeris::projection::sinusoidal_forward(lambda, beta, radius);
            expect_true("sinusoidal forward succeeds", projected.ok());
            if (!projected.ok()) {
                continue;
            }

            const auto inverse = aeris::projection::sinusoidal_inverse(
                projected.value.x,
                projected.value.y,
                radius
            );
            expect_true("sinusoidal inverse succeeds", inverse.ok());
            if (inverse.ok()) {
                expect_near("sinusoidal longitude round-trip", inverse.value.longitude_rad, lambda, 8e-15);
                expect_near("sinusoidal latitude round-trip", inverse.value.latitude_rad, beta, 2e-15);
            }
        }
    }
}

void test_mollweide_auxiliary_equation() {
    for (int latitude_deg = -89; latitude_deg <= 89; ++latitude_deg) {
        const double beta = static_cast<double>(latitude_deg) * aeris::geo::kPi / 180.0;
        const auto theta = aeris::projection::mollweide_auxiliary_angle(beta);
        expect_true("Mollweide theta solver succeeds", theta.ok());
        if (!theta.ok()) {
            continue;
        }

        const double residual =
            2.0 * theta.value + std::sin(2.0 * theta.value) - aeris::geo::kPi * std::sin(beta);
        expect_near("Mollweide theta equation residual", residual, 0.0, 2e-14);
    }
}

void test_mollweide_round_trip() {
    const double radius = aeris::geo::authalic_radius_m();

    for (int latitude_deg = -80; latitude_deg <= 80; latitude_deg += 10) {
        for (int longitude_deg = -170; longitude_deg <= 170; longitude_deg += 17) {
            const double beta = static_cast<double>(latitude_deg) * aeris::geo::kPi / 180.0;
            const double lambda = static_cast<double>(longitude_deg) * aeris::geo::kPi / 180.0;
            const auto projected = aeris::projection::mollweide_forward(lambda, beta, radius);
            expect_true("Mollweide forward succeeds", projected.ok());
            if (!projected.ok()) {
                continue;
            }

            const auto inverse = aeris::projection::mollweide_inverse(
                projected.value.x,
                projected.value.y,
                radius
            );
            expect_true("Mollweide inverse succeeds", inverse.ok());
            if (inverse.ok()) {
                expect_near("Mollweide longitude round-trip", inverse.value.longitude_rad, lambda, 2e-13);
                expect_near("Mollweide latitude round-trip", inverse.value.latitude_rad, beta, 2e-14);
            }
        }
    }
}

void test_pole_inverse_is_explicitly_indeterminate() {
    const double radius = aeris::geo::authalic_radius_m();

    const auto sinusoidal = aeris::projection::sinusoidal_inverse(0.0, radius * aeris::geo::kHalfPi, radius);
    expect_true(
        "sinusoidal pole longitude is marked indeterminate",
        sinusoidal.error == aeris::geo::MathError::indeterminate_coordinate
    );

    const double mollweide_pole_y = 1.4142135623730950488 * radius;
    const auto mollweide = aeris::projection::mollweide_inverse(0.0, mollweide_pole_y, radius);
    expect_true(
        "Mollweide pole longitude is marked indeterminate",
        mollweide.error == aeris::geo::MathError::indeterminate_coordinate
    );
}

void test_invalid_input_rejection() {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    expect_true(
        "sinusoidal NaN rejected",
        aeris::projection::sinusoidal_forward(nan, 0.0).error == aeris::geo::MathError::non_finite_input
    );
    expect_true(
        "Mollweide invalid latitude rejected",
        aeris::projection::mollweide_forward(0.0, aeris::geo::kHalfPi + 1e-6).error ==
            aeris::geo::MathError::latitude_out_of_range
    );
}

}  // namespace

int main() {
    test_sinusoidal_origin_and_round_trip();
    test_mollweide_auxiliary_equation();
    test_mollweide_round_trip();
    test_pole_inverse_is_explicitly_indeterminate();
    test_invalid_input_rejection();

    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return EXIT_FAILURE;
    }

    std::cout << "projection_primitives: PASS\n";
    return EXIT_SUCCESS;
}
