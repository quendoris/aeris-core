// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/geo/rotation.hpp"

#include <cmath>
#include <limits>

namespace aeris::geo {
namespace {

[[nodiscard]] double normalized_longitude(const double longitude_rad) noexcept {
    double value = std::remainder(longitude_rad, 2.0 * kPi);
    if (value <= -kPi) {
        value += 2.0 * kPi;
    }
    return value;
}

}  // namespace

double dot(const Vec3 a, const Vec3 b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

double norm(const Vec3 vector) noexcept {
    return std::sqrt(dot(vector, vector));
}

double determinant(const Mat3& matrix) noexcept {
    return
        matrix(0, 0) * (matrix(1, 1) * matrix(2, 2) - matrix(1, 2) * matrix(2, 1)) -
        matrix(0, 1) * (matrix(1, 0) * matrix(2, 2) - matrix(1, 2) * matrix(2, 0)) +
        matrix(0, 2) * (matrix(1, 0) * matrix(2, 1) - matrix(1, 1) * matrix(2, 0));
}

Mat3 transpose(const Mat3& matrix) noexcept {
    return {{
        matrix(0, 0), matrix(1, 0), matrix(2, 0),
        matrix(0, 1), matrix(1, 1), matrix(2, 1),
        matrix(0, 2), matrix(1, 2), matrix(2, 2),
    }};
}

Mat3 multiply(const Mat3& left, const Mat3& right) noexcept {
    Mat3 result{{
        0.0, 0.0, 0.0,
        0.0, 0.0, 0.0,
        0.0, 0.0, 0.0,
    }};

    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            double value = 0.0;
            for (int index = 0; index < 3; ++index) {
                value += left(row, index) * right(index, column);
            }
            result.m[static_cast<std::size_t>(row * 3 + column)] = value;
        }
    }

    return result;
}

Vec3 apply(const Mat3& matrix, const Vec3 vector) noexcept {
    return {
        matrix(0, 0) * vector.x + matrix(0, 1) * vector.y + matrix(0, 2) * vector.z,
        matrix(1, 0) * vector.x + matrix(1, 1) * vector.y + matrix(1, 2) * vector.z,
        matrix(2, 0) * vector.x + matrix(2, 1) * vector.y + matrix(2, 2) * vector.z,
    };
}

Mat3 rotation_x(const double angle_rad) noexcept {
    const double c = std::cos(angle_rad);
    const double s = std::sin(angle_rad);
    return {{
        1.0, 0.0, 0.0,
        0.0, c, -s,
        0.0, s, c,
    }};
}

Mat3 rotation_y(const double angle_rad) noexcept {
    const double c = std::cos(angle_rad);
    const double s = std::sin(angle_rad);
    return {{
        c, 0.0, s,
        0.0, 1.0, 0.0,
        -s, 0.0, c,
    }};
}

Mat3 rotation_z(const double angle_rad) noexcept {
    const double c = std::cos(angle_rad);
    const double s = std::sin(angle_rad);
    return {{
        c, -s, 0.0,
        s, c, 0.0,
        0.0, 0.0, 1.0,
    }};
}

Vec3 lonlat_to_unit_vector(
    const double longitude_rad,
    const double latitude_rad
) noexcept {
    if (!std::isfinite(longitude_rad) || !std::isfinite(latitude_rad) ||
        latitude_rad < -kHalfPi || latitude_rad > kHalfPi) {
        const double nan = std::numeric_limits<double>::quiet_NaN();
        return {nan, nan, nan};
    }

    const double cos_latitude = std::cos(latitude_rad);
    return {
        cos_latitude * std::cos(longitude_rad),
        cos_latitude * std::sin(longitude_rad),
        std::sin(latitude_rad),
    };
}

LonLatResult unit_vector_to_lonlat(const Vec3 vector) noexcept {
    if (!std::isfinite(vector.x) || !std::isfinite(vector.y) || !std::isfinite(vector.z)) {
        return {{}, MathError::non_finite_input};
    }

    const double vector_norm = norm(vector);
    if (!std::isfinite(vector_norm) || vector_norm <= 0.0) {
        return {{}, MathError::numerical_domain_error};
    }

    const double x = vector.x / vector_norm;
    const double y = vector.y / vector_norm;
    const double z = vector.z / vector_norm;
    const double horizontal = std::hypot(x, y);
    const double latitude = std::atan2(z, horizontal);

    constexpr double pole_floor = 64.0 * std::numeric_limits<double>::epsilon();
    if (horizontal <= pole_floor) {
        return {{0.0, latitude}, MathError::indeterminate_coordinate};
    }

    const double longitude = normalized_longitude(std::atan2(y, x));
    return {{longitude, latitude}, MathError::none};
}

}  // namespace aeris::geo
