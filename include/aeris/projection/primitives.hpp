// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "aeris/geo/wgs84.hpp"
#include "aeris/geometry/planar.hpp"

namespace aeris::projection {

using PlanarPoint = geometry::PlanarPoint;

struct SphericalPoint final {
    double longitude_rad = 0.0;
    double latitude_rad = 0.0;
};

struct PlanarResult final {
    PlanarPoint value{};
    geo::MathError error = geo::MathError::none;

    [[nodiscard]] constexpr bool ok() const noexcept {
        return error == geo::MathError::none;
    }
};

struct SphericalResult final {
    SphericalPoint value{};
    geo::MathError error = geo::MathError::none;

    [[nodiscard]] constexpr bool ok() const noexcept {
        return error == geo::MathError::none;
    }
};

[[nodiscard]] PlanarResult sinusoidal_forward(
    double longitude_delta_rad,
    double authalic_latitude_rad,
    double radius_m = geo::authalic_radius_m()
) noexcept;

[[nodiscard]] SphericalResult sinusoidal_inverse(
    double x,
    double y,
    double radius_m = geo::authalic_radius_m()
) noexcept;

[[nodiscard]] geo::ScalarResult mollweide_auxiliary_angle(double authalic_latitude_rad) noexcept;

[[nodiscard]] PlanarResult mollweide_forward(
    double longitude_delta_rad,
    double authalic_latitude_rad,
    double radius_m = geo::authalic_radius_m()
) noexcept;

[[nodiscard]] SphericalResult mollweide_inverse(
    double x,
    double y,
    double radius_m = geo::authalic_radius_m()
) noexcept;

}  // namespace aeris::projection
