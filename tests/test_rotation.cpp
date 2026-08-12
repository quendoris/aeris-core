// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/geo/rotation.hpp"

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

void test_known_axis_rotations() {
    using namespace aeris::geo;

    const Vec3 x{1.0, 0.0, 0.0};
    const Vec3 y{0.0, 1.0, 0.0};
    const Vec3 z{0.0, 0.0, 1.0};

    const Vec3 around_z = apply(rotation_z(kHalfPi), x);
    expect_near("Rz quarter turn x", around_z.x, 0.0, 2e-16);
    expect_near("Rz quarter turn y", around_z.y, 1.0, 2e-16);
    expect_near("Rz quarter turn z", around_z.z, 0.0, 0.0);

    const Vec3 around_x = apply(rotation_x(kHalfPi), y);
    expect_near("Rx quarter turn x", around_x.x, 0.0, 0.0);
    expect_near("Rx quarter turn y", around_x.y, 0.0, 2e-16);
    expect_near("Rx quarter turn z", around_x.z, 1.0, 2e-16);

    const Vec3 around_y = apply(rotation_y(kHalfPi), z);
    expect_near("Ry quarter turn x", around_y.x, 1.0, 2e-16);
    expect_near("Ry quarter turn y", around_y.y, 0.0, 0.0);
    expect_near("Ry quarter turn z", around_y.z, 0.0, 2e-16);
}

void test_composed_rotation_is_orthonormal() {
    using namespace aeris::geo;

    const Mat3 rotation = multiply(
        rotation_z(0.731),
        multiply(rotation_y(-1.117), rotation_x(0.293))
    );
    const Mat3 product = multiply(transpose(rotation), rotation);

    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            expect_near(
                "rotation transpose product",
                product(row, column),
                row == column ? 1.0 : 0.0,
                5e-16
            );
        }
    }

    expect_near("proper rotation determinant", determinant(rotation), 1.0, 6e-16);
}

void test_norm_preservation_and_inverse() {
    using namespace aeris::geo;

    const Mat3 rotation = multiply(rotation_z(-0.811), rotation_y(0.447));
    const Mat3 inverse = transpose(rotation);

    for (int latitude_deg = -80; latitude_deg <= 80; latitude_deg += 8) {
        for (int longitude_deg = -180; longitude_deg <= 180; longitude_deg += 15) {
            const double longitude = static_cast<double>(longitude_deg) * kPi / 180.0;
            const double latitude = static_cast<double>(latitude_deg) * kPi / 180.0;
            const Vec3 original = lonlat_to_unit_vector(longitude, latitude);
            const Vec3 rotated = apply(rotation, original);
            const Vec3 recovered = apply(inverse, rotated);

            expect_near("rotation preserves unit norm", norm(rotated), 1.0, 5e-16);
            expect_near("inverse recovers x", recovered.x, original.x, 7e-16);
            expect_near("inverse recovers y", recovered.y, original.y, 7e-16);
            expect_near("inverse recovers z", recovered.z, original.z, 7e-16);
        }
    }
}

void test_lonlat_vector_round_trip() {
    using namespace aeris::geo;

    for (int latitude_deg = -85; latitude_deg <= 85; latitude_deg += 5) {
        for (int longitude_deg = -175; longitude_deg <= 175; longitude_deg += 10) {
            const double longitude = static_cast<double>(longitude_deg) * kPi / 180.0;
            const double latitude = static_cast<double>(latitude_deg) * kPi / 180.0;
            const auto recovered = unit_vector_to_lonlat(lonlat_to_unit_vector(longitude, latitude));
            expect_true("lonlat vector round trip succeeds", recovered.ok());
            if (recovered.ok()) {
                expect_near("lonlat round trip longitude", recovered.value.longitude_rad, longitude, 3e-15);
                expect_near("lonlat round trip latitude", recovered.value.latitude_rad, latitude, 3e-15);
            }
        }
    }

    const auto north = unit_vector_to_lonlat({0.0, 0.0, 1.0});
    expect_true(
        "pole longitude is explicitly indeterminate",
        north.error == MathError::indeterminate_coordinate
    );
    expect_near("pole latitude survives", north.value.latitude_rad, kHalfPi, 0.0);

    const auto zero = unit_vector_to_lonlat({0.0, 0.0, 0.0});
    expect_true("zero vector rejected", zero.error == MathError::numerical_domain_error);

    const double nan = std::numeric_limits<double>::quiet_NaN();
    const auto non_finite = unit_vector_to_lonlat({nan, 0.0, 0.0});
    expect_true("non-finite vector rejected", non_finite.error == MathError::non_finite_input);
}

}  // namespace

int main() {
    test_known_axis_rotations();
    test_composed_rotation_is_orthonormal();
    test_norm_preservation_and_inverse();
    test_lonlat_vector_round_trip();

    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return EXIT_FAILURE;
    }

    std::cout << "spherical_rotation: PASS\n";
    return EXIT_SUCCESS;
}
