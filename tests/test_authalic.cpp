// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/geo/wgs84.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>

namespace {

int failures = 0;

void fail(const std::string_view name, const double actual, const double expected, const double tolerance) {
    ++failures;
    std::cerr << "FAIL " << name << ": actual=" << actual
              << " expected=" << expected << " tolerance=" << tolerance << '\n';
}

void expect_near(
    const std::string_view name,
    const double actual,
    const double expected,
    const double tolerance
) {
    if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance) {
        fail(name, actual, expected, tolerance);
    }
}

void expect_true(const std::string_view name, const bool condition) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL " << name << '\n';
    }
}

[[nodiscard]] double authalic_q_derivative(const double phi) {
    const double sin_phi = std::sin(phi);
    const double cos_phi = std::cos(phi);
    const double denominator =
        1.0 - aeris::geo::Wgs84::eccentricity_squared * sin_phi * sin_phi;

    return
        2.0 * (1.0 - aeris::geo::Wgs84::eccentricity_squared) * cos_phi /
        (denominator * denominator);
}

[[nodiscard]] double inverse_round_trip_tolerance(const double phi) {
    // A fixed angular tolerance becomes mathematically dishonest near a pole:
    // q(phi) flattens as dq/dphi -> 0, so a few unavoidable binary64 ULPs in
    // q/q_p map to a larger angular interval. Bound that interval explicitly
    // from the local derivative instead of globally weakening the test.
    constexpr double base_angular_tolerance = 2e-14;
    constexpr double q_ulp_budget = 4.0 * std::numeric_limits<double>::epsilon();

    const double q_tolerance = q_ulp_budget * std::abs(aeris::geo::authalic_q_pole());
    const double derivative = std::abs(authalic_q_derivative(phi));
    if (derivative == 0.0) {
        return std::numeric_limits<double>::infinity();
    }

    return std::max(base_angular_tolerance, q_tolerance / derivative);
}

void test_wgs84_constants() {
    expect_near(
        "WGS84 eccentricity squared",
        aeris::geo::Wgs84::eccentricity_squared,
        0.0066943799901413165,
        1e-18
    );

    expect_near(
        "WGS84 eccentricity",
        aeris::geo::wgs84_eccentricity(),
        0.08181919084262149,
        1e-17
    );
}

void test_authalic_radius() {
    expect_near(
        "WGS84 authalic q pole",
        aeris::geo::authalic_q_pole(),
        1.9955310875028376,
        2e-15
    );

    expect_near(
        "WGS84 authalic radius",
        aeris::geo::authalic_radius_m(),
        6371007.180918476,
        1e-6
    );
}

void test_authalic_latitude_reference_points() {
    struct Case final {
        double geodetic_deg;
        double authalic_deg;
    };

    constexpr Case cases[] = {
        {0.0, 0.0},
        {30.0, 29.888997034459564},
        {45.0, 44.87170287343392},
        {60.0, 59.88878556988508},
        {89.0, 88.99551395786139},
        {-30.0, -29.88899703445959},
    };

    for (const Case& item : cases) {
        const double phi = item.geodetic_deg * aeris::geo::kPi / 180.0;
        const auto result = aeris::geo::authalic_latitude(phi);
        expect_true("authalic latitude reference point returns success", result.ok());
        if (result.ok()) {
            const double actual_deg = result.value * 180.0 / aeris::geo::kPi;
            expect_near("authalic latitude reference point", actual_deg, item.authalic_deg, 2e-12);
        }
    }
}

void test_symmetry_and_poles() {
    const auto north = aeris::geo::authalic_latitude(aeris::geo::kHalfPi);
    const auto south = aeris::geo::authalic_latitude(-aeris::geo::kHalfPi);
    expect_true("north pole succeeds", north.ok());
    expect_true("south pole succeeds", south.ok());
    expect_near("north pole is preserved", north.value, aeris::geo::kHalfPi, 0.0);
    expect_near("south pole is preserved", south.value, -aeris::geo::kHalfPi, 0.0);

    for (int degree = 1; degree < 90; ++degree) {
        const double phi = static_cast<double>(degree) * aeris::geo::kPi / 180.0;
        const auto positive = aeris::geo::authalic_latitude(phi);
        const auto negative = aeris::geo::authalic_latitude(-phi);
        expect_true("positive latitude succeeds", positive.ok());
        expect_true("negative latitude succeeds", negative.ok());
        if (positive.ok() && negative.ok()) {
            expect_near("authalic latitude is odd", positive.value, -negative.value, 2e-15);
        }
    }
}

void test_monotonicity() {
    double previous = -aeris::geo::kHalfPi;
    for (int i = -900; i <= 900; ++i) {
        const double phi = static_cast<double>(i) * aeris::geo::kPi / 1800.0;
        const auto result = aeris::geo::authalic_latitude(phi);
        expect_true("monotonicity sample succeeds", result.ok());
        if (result.ok()) {
            expect_true("authalic latitude is monotonic", result.value >= previous);
            previous = result.value;
        }
    }
}

void test_authalic_inverse_round_trip() {
    constexpr double q_ulp_budget = 4.0 * std::numeric_limits<double>::epsilon();
    const double q_tolerance = q_ulp_budget * std::abs(aeris::geo::authalic_q_pole());

    for (int i = -899; i <= 899; ++i) {
        const double phi = static_cast<double>(i) * aeris::geo::kPi / 1800.0;
        const auto beta = aeris::geo::authalic_latitude(phi);
        expect_true("authalic forward for inverse round-trip succeeds", beta.ok());
        if (!beta.ok()) {
            continue;
        }

        const auto recovered = aeris::geo::geodetic_latitude_from_authalic(beta.value);
        expect_true("authalic inverse succeeds", recovered.ok());
        if (recovered.ok()) {
            expect_near(
                "authalic inverse round-trip",
                recovered.value,
                phi,
                inverse_round_trip_tolerance(phi)
            );
            expect_near(
                "authalic inverse preserves q within binary64 budget",
                aeris::geo::authalic_q(recovered.value),
                aeris::geo::authalic_q(phi),
                q_tolerance
            );
        }
    }

    const auto north = aeris::geo::geodetic_latitude_from_authalic(aeris::geo::kHalfPi);
    const auto south = aeris::geo::geodetic_latitude_from_authalic(-aeris::geo::kHalfPi);
    expect_true("inverse north pole succeeds", north.ok());
    expect_true("inverse south pole succeeds", south.ok());
    expect_near("inverse north pole preserved", north.value, aeris::geo::kHalfPi, 0.0);
    expect_near("inverse south pole preserved", south.value, -aeris::geo::kHalfPi, 0.0);
}

void test_authalic_inverse_arbitrary_valid_latitudes() {
    // Projection-frame rotations produce perfectly valid authalic latitudes that
    // are not themselves the output of one of the coarse geodetic samples above.
    // Tiny values near the equator are particularly important: the bounded
    // inverse must return the already-converged midpoint rather than report a
    // false non-convergence merely because two binary64 bracket endpoints remain.
    constexpr std::array<double, 10> beta_samples{
        -0.876673,
        -0.421326,
        -1.0e-14,
        -1.0e-16,
        1.0e-16,
        1.0e-14,
        0.421326,
        0.715875,
        0.876673,
        1.2,
    };

    for (const double beta : beta_samples) {
        const auto recovered = aeris::geo::geodetic_latitude_from_authalic(beta);
        expect_true("arbitrary authalic inverse succeeds", recovered.ok());
        if (!recovered.ok()) {
            continue;
        }

        const auto round_trip = aeris::geo::authalic_latitude(recovered.value);
        expect_true("arbitrary authalic inverse forward check succeeds", round_trip.ok());
        if (round_trip.ok()) {
            expect_near(
                "arbitrary authalic latitude round-trip",
                round_trip.value,
                beta,
                2e-14
            );
        }
    }
}

void test_invalid_inputs_fail_closed() {
    const auto nan = aeris::geo::authalic_latitude(std::numeric_limits<double>::quiet_NaN());
    expect_true("NaN rejected", nan.error == aeris::geo::MathError::non_finite_input);

    const auto infinity = aeris::geo::authalic_latitude(std::numeric_limits<double>::infinity());
    expect_true("infinity rejected", infinity.error == aeris::geo::MathError::non_finite_input);

    const auto high = aeris::geo::authalic_latitude(aeris::geo::kHalfPi + 1e-12);
    expect_true("latitude above north pole rejected", high.error == aeris::geo::MathError::latitude_out_of_range);

    const auto low = aeris::geo::authalic_latitude(-aeris::geo::kHalfPi - 1e-12);
    expect_true("latitude below south pole rejected", low.error == aeris::geo::MathError::latitude_out_of_range);
}

}  // namespace

int main() {
    test_wgs84_constants();
    test_authalic_radius();
    test_authalic_latitude_reference_points();
    test_symmetry_and_poles();
    test_monotonicity();
    test_authalic_inverse_round_trip();
    test_authalic_inverse_arbitrary_valid_latitudes();
    test_invalid_inputs_fail_closed();

    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return EXIT_FAILURE;
    }

    std::cout << "authalic_math: PASS\n";
    return EXIT_SUCCESS;
}
