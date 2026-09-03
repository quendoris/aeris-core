// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/projection/sinu_mollweide.hpp"

#include <array>
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

[[nodiscard]] double radians(const double degrees) noexcept {
    return degrees * aeris::geo::kPi / 180.0;
}

void test_raw_join_is_continuous() {
    const double radius = aeris::geo::authalic_radius_m();
    constexpr std::array<double, 5> longitudes{
        -aeris::geo::kPi,
        -1.7,
        0.0,
        1.7,
        aeris::geo::kPi,
    };
    const double beta = -aeris::projection::kSinuMollweideTransitionLatitudeRad;

    for (const double longitude : longitudes) {
        const auto sinu = aeris::projection::sinusoidal_forward(longitude, beta, radius);
        auto moll = aeris::projection::mollweide_forward(longitude, beta, radius);
        expect_true("join Sinusoidal succeeds", sinu.ok());
        expect_true("join Mollweide succeeds", moll.ok());
        if (!sinu.ok() || !moll.ok()) continue;

        moll.value.y += aeris::projection::kSinuMollweideNorthingOffsetRatio * radius;
        expect_near("join x continuity", moll.value.x, sinu.value.x, 5e-5);
        expect_near("join y continuity", moll.value.y, sinu.value.y, 5e-7);
    }
}

void test_raw_round_trip() {
    const double radius = aeris::geo::authalic_radius_m();
    for (int latitude_deg = -80; latitude_deg <= 80; latitude_deg += 10) {
        for (int longitude_deg = -170; longitude_deg <= 170; longitude_deg += 17) {
            const double beta = radians(static_cast<double>(latitude_deg));
            const double longitude = radians(static_cast<double>(longitude_deg));
            const auto projected = aeris::projection::sinu_mollweide_forward(
                longitude,
                beta,
                radius
            );
            expect_true("raw Sinu-Mollweide forward succeeds", projected.ok());
            if (!projected.ok()) continue;

            const auto inverse = aeris::projection::sinu_mollweide_inverse(
                projected.value.x,
                projected.value.y,
                radius
            );
            expect_true("raw Sinu-Mollweide inverse succeeds", inverse.ok());
            if (!inverse.ok()) continue;

            expect_near("raw longitude round-trip", inverse.value.longitude_rad, longitude, 4e-13);
            expect_near("raw latitude round-trip", inverse.value.latitude_rad, beta, 4e-14);
        }
    }
}

void test_philbrick_center_and_round_trip() {
    const double radius = aeris::geo::authalic_radius_m();
    const auto center = aeris::projection::philbrick_sinu_mollweide_forward_wgs84(
        aeris::projection::kPhilbrickCenterLongitudeRad,
        aeris::projection::kPhilbrickCenterGeodeticLatitudeRad,
        radius
    );
    expect_true("Philbrick center forward succeeds", center.ok());
    if (center.ok()) {
        expect_near("Philbrick center x", center.value.x, 0.0, 5e-7);
        expect_near(
            "Philbrick center raw y offset",
            center.value.y,
            aeris::projection::kSinuMollweideNorthingOffsetRatio * radius,
            5e-7
        );
    }

    constexpr std::array<std::array<double, 2>, 7> samples{{
        {{20.0, 55.0}},
        {{-100.0, 0.0}},
        {{0.0, 0.0}},
        {{120.0, -50.0}},
        {{80.0, -40.0}},
        {{150.0, 15.0}},
        {{-60.0, -65.0}},
    }};

    for (const auto sample : samples) {
        const double longitude = radians(sample[0]);
        const double latitude = radians(sample[1]);
        const auto projected = aeris::projection::philbrick_sinu_mollweide_forward_wgs84(
            longitude,
            latitude,
            radius
        );
        expect_true("Philbrick forward succeeds", projected.ok());
        if (!projected.ok()) continue;

        const auto inverse = aeris::projection::philbrick_sinu_mollweide_inverse_wgs84(
            projected.value.x,
            projected.value.y,
            radius
        );
        expect_true("Philbrick inverse succeeds", inverse.ok());
        if (!inverse.ok()) continue;

        double longitude_error = std::remainder(
            inverse.value.longitude_rad - longitude,
            2.0 * aeris::geo::kPi
        );
        expect_near("Philbrick longitude round-trip", longitude_error, 0.0, 2e-12);
        expect_near("Philbrick latitude round-trip", inverse.value.latitude_rad, latitude, 2e-12);
    }
}

[[nodiscard]] double wgs84_area_density(const double geodetic_latitude_rad) noexcept {
    const double sin_latitude = std::sin(geodetic_latitude_rad);
    const double denominator =
        1.0 - aeris::geo::Wgs84::eccentricity_squared * sin_latitude * sin_latitude;
    const double a2 =
        aeris::geo::Wgs84::semi_major_axis_m * aeris::geo::Wgs84::semi_major_axis_m;
    return a2 * (1.0 - aeris::geo::Wgs84::eccentricity_squared) *
        std::cos(geodetic_latitude_rad) / (denominator * denominator);
}

[[nodiscard]] double numerical_wgs84_jacobian(
    const double longitude_rad,
    const double latitude_rad
) {
    constexpr double h = 2e-6;
    const auto lambda_plus = aeris::projection::philbrick_sinu_mollweide_forward_wgs84(
        longitude_rad + h,
        latitude_rad
    );
    const auto lambda_minus = aeris::projection::philbrick_sinu_mollweide_forward_wgs84(
        longitude_rad - h,
        latitude_rad
    );
    const auto latitude_plus = aeris::projection::philbrick_sinu_mollweide_forward_wgs84(
        longitude_rad,
        latitude_rad + h
    );
    const auto latitude_minus = aeris::projection::philbrick_sinu_mollweide_forward_wgs84(
        longitude_rad,
        latitude_rad - h
    );

    expect_true("Jacobian lambda+ succeeds", lambda_plus.ok());
    expect_true("Jacobian lambda- succeeds", lambda_minus.ok());
    expect_true("Jacobian phi+ succeeds", latitude_plus.ok());
    expect_true("Jacobian phi- succeeds", latitude_minus.ok());
    if (!lambda_plus.ok() || !lambda_minus.ok() ||
        !latitude_plus.ok() || !latitude_minus.ok()) {
        return std::numeric_limits<double>::quiet_NaN();
    }

    const double dx_dlambda =
        (lambda_plus.value.x - lambda_minus.value.x) / (2.0 * h);
    const double dy_dlambda =
        (lambda_plus.value.y - lambda_minus.value.y) / (2.0 * h);
    const double dx_dphi =
        (latitude_plus.value.x - latitude_minus.value.x) / (2.0 * h);
    const double dy_dphi =
        (latitude_plus.value.y - latitude_minus.value.y) / (2.0 * h);
    return std::abs(dx_dlambda * dy_dphi - dx_dphi * dy_dlambda);
}

void test_philbrick_preserves_wgs84_area_locally() {
    constexpr std::array<std::array<double, 2>, 4> samples{{
        {{20.0, 55.0}},
        {{-100.0, 0.0}},
        {{0.0, 0.0}},
        {{120.0, -50.0}},
    }};

    for (const auto sample : samples) {
        const double longitude = radians(sample[0]);
        const double latitude = radians(sample[1]);
        const double actual = numerical_wgs84_jacobian(longitude, latitude);
        const double expected = wgs84_area_density(latitude);
        expect_near("Philbrick WGS84 equal-area Jacobian ratio", actual / expected, 1.0, 3e-7);
    }
}

void test_invalid_input_rejection() {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    expect_true(
        "raw Sinu-Mollweide NaN rejected",
        aeris::projection::sinu_mollweide_forward(nan, 0.0).error ==
            aeris::geo::MathError::non_finite_input
    );
    expect_true(
        "Philbrick invalid WGS84 latitude rejected",
        aeris::projection::philbrick_sinu_mollweide_forward_wgs84(
            0.0,
            aeris::geo::kHalfPi + 1e-6
        ).error == aeris::geo::MathError::latitude_out_of_range
    );
}

}  // namespace

int main() {
    test_raw_join_is_continuous();
    test_raw_round_trip();
    test_philbrick_center_and_round_trip();
    test_philbrick_preserves_wgs84_area_locally();
    test_invalid_input_rejection();

    if (failures != 0) {
        std::cerr << failures << " Sinu-Mollweide assertion(s) failed\n";
        return EXIT_FAILURE;
    }

    std::cout << "sinu_mollweide_projection: PASS\n";
    return EXIT_SUCCESS;
}
