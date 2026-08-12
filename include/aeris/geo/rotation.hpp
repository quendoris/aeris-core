// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "aeris/geo/wgs84.hpp"

#include <array>

namespace aeris::geo {

struct Vec3 final {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

struct Mat3 final {
    std::array<double, 9> m{
        1.0, 0.0, 0.0,
        0.0, 1.0, 0.0,
        0.0, 0.0, 1.0,
    };

    [[nodiscard]] constexpr double operator()(const int row, const int column) const noexcept {
        return m[static_cast<std::size_t>(row * 3 + column)];
    }
};

struct LonLat final {
    double longitude_rad = 0.0;
    double latitude_rad = 0.0;
};

struct LonLatResult final {
    LonLat value{};
    MathError error = MathError::none;

    [[nodiscard]] constexpr bool ok() const noexcept {
        return error == MathError::none;
    }
};

[[nodiscard]] double dot(Vec3 a, Vec3 b) noexcept;
[[nodiscard]] double norm(Vec3 vector) noexcept;
[[nodiscard]] double determinant(const Mat3& matrix) noexcept;
[[nodiscard]] Mat3 transpose(const Mat3& matrix) noexcept;
[[nodiscard]] Mat3 multiply(const Mat3& left, const Mat3& right) noexcept;
[[nodiscard]] Vec3 apply(const Mat3& matrix, Vec3 vector) noexcept;

[[nodiscard]] Mat3 rotation_x(double angle_rad) noexcept;
[[nodiscard]] Mat3 rotation_y(double angle_rad) noexcept;
[[nodiscard]] Mat3 rotation_z(double angle_rad) noexcept;

[[nodiscard]] Vec3 lonlat_to_unit_vector(double longitude_rad, double latitude_rad) noexcept;
[[nodiscard]] LonLatResult unit_vector_to_lonlat(Vec3 vector) noexcept;

}  // namespace aeris::geo
