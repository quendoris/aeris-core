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

template <typename Projector>
double numerical_jacobian(
    Projector projector,
    const double longitude,
    const double latitude,
    const double radius
) {
    constexpr double h = 1e-5;

    const auto lambda_plus = projector(longitude + h, latitude, radius);
    const auto lambda_minus = projector(longitude - h, latitude, radius);
    const auto latitude_plus = projector(longitude, latitude + h, radius);
    const auto latitude_minus = projector(longitude, latitude - h, radius);

    expect_true("Jacobian lambda+ projection succeeds", lambda_plus.ok());
    expect_true("Jacobian lambda- projection succeeds", lambda_minus.ok());
    expect_true("Jacobian beta+ projection succeeds", latitude_plus.ok());
    expect_true("Jacobian beta- projection succeeds", latitude_minus.ok());
    if (!lambda_plus.ok() || !lambda_minus.ok() ||
        !latitude_plus.ok() || !latitude_minus.ok()) {
        return std::numeric_limits<double>::quiet_NaN();
    }

    const double dx_dlambda = (lambda_plus.value.x - lambda_minus.value.x) / (2.0 * h);
    const double dy_dlambda = (lambda_plus.value.y - lambda_minus.value.y) / (2.0 * h);
    const double dx_dbeta = (latitude_plus.value.x - latitude_minus.value.x) / (2.0 * h);
    const double dy_dbeta = (latitude_plus.value.y - latitude_minus.value.y) / (2.0 * h);

    return std::abs(dx_dlambda * dy_dbeta - dx_dbeta * dy_dlambda);
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

void test_local_area_jacobians() {
    using aeris::projection::mollweide_forward;
    using aeris::projection::sinusoidal_forward;

    const double radius = aeris::geo::authalic_radius_m();
    constexpr int latitudes_deg[] = {-80, -60, -30, 0, 30, 60, 80};
    constexpr double longitudes[] = {-2.1, -0.73, 0.0, 0.73, 2.1};

    for (const int latitude_deg : latitudes_deg) {
        const double beta = static_cast<double>(latitude_deg) * aeris::geo::kPi / 180.0;
        const double expected = radius * radius * std::cos(beta);

        for (const double lambda : longitudes) {
            const double sinusoidal = numerical_jacobian(sinusoidal_forward, lambda, beta, radius);
            const double mollweide = numerical_jacobian(mollweide_forward, lambda, beta, radius);

            expect_near("sinusoidal equal-area Jacobian ratio", sinusoidal / expected, 1.0, 5e-10);
            expect_near("Mollweide equal-area Jacobian ratio", mollweide / expected, 1.0, 5e-10);
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
    expect_true(
        "sinusoidal negative radius rejected as numerical domain",
        aeris::projection::sinusoidal_forward(0.0, 0.0, -1.0).error ==
            aeris::geo::MathError::numerical_domain_error
    );
    expect_true(
        "Mollweide zero radius rejected as numerical domain",
        aeris::projection::mollweide_forward(0.0, 0.0, 0.0).error ==
            aeris::geo::MathError::numerical_domain_error
    );
}

}  // namespace

int main() {
    test_sinusoidal_origin_and_round_trip();
    test_mollweide_auxiliary_equation();
    test_mollweide_round_trip();
    test_local_area_jacobians();
    test_pole_inverse_is_explicitly_indeterminate();
    test_invalid_input_rejection();

    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return EXIT_FAILURE;
    }

    std::cout << "projection_primitives: PASS\n";
    return EXIT_SUCCESS;
}
