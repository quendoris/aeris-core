// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/projection/sinu_mollweide.hpp"

#include <cmath>

namespace aeris::projection {
namespace {

[[nodiscard]] bool usable_lonlat_result(const geo::LonLatResult& result) noexcept {
    return result.ok() || result.error == geo::MathError::indeterminate_coordinate;
}

[[nodiscard]] bool usable_spherical_result(const SphericalResult& result) noexcept {
    return result.ok() || result.error == geo::MathError::indeterminate_coordinate;
}

}  // namespace

geo::Mat3 philbrick_world_to_projection_matrix() noexcept {
    static const geo::Mat3 matrix = []() noexcept {
        const auto center_beta = geo::authalic_latitude(
            kPhilbrickCenterGeodeticLatitudeRad
        );
        if (!center_beta.ok()) {
            return geo::Mat3{};
        }
        return geo::multiply(
            geo::rotation_y(center_beta.value),
            geo::rotation_z(-kPhilbrickCenterLongitudeRad)
        );
    }();
    return matrix;
}

PlanarResult sinu_mollweide_forward(
    const double longitude_rad,
    const double authalic_latitude_rad,
    const double radius_m
) noexcept {
    if (authalic_latitude_rad <= -kSinuMollweideTransitionLatitudeRad) {
        return sinusoidal_forward(longitude_rad, authalic_latitude_rad, radius_m);
    }

    PlanarResult result = mollweide_forward(
        longitude_rad,
        authalic_latitude_rad,
        radius_m
    );
    if (result.ok()) {
        result.value.y += kSinuMollweideNorthingOffsetRatio * radius_m;
    }
    return result;
}

SphericalResult sinu_mollweide_inverse(
    const double x,
    const double y,
    const double radius_m
) noexcept {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(radius_m)) {
        return {{}, geo::MathError::non_finite_input};
    }
    if (!(radius_m > 0.0)) {
        return {{}, geo::MathError::numerical_domain_error};
    }

    const double join_y = -kSinuMollweideTransitionLatitudeRad * radius_m;
    if (y <= join_y) {
        return sinusoidal_inverse(x, y, radius_m);
    }
    return mollweide_inverse(
        x,
        y - kSinuMollweideNorthingOffsetRatio * radius_m,
        radius_m
    );
}

PlanarResult philbrick_sinu_mollweide_forward_wgs84(
    const double longitude_rad,
    const double geodetic_latitude_rad,
    const double radius_m
) noexcept {
    if (!std::isfinite(longitude_rad) || !std::isfinite(geodetic_latitude_rad) ||
        !std::isfinite(radius_m)) {
        return {{}, geo::MathError::non_finite_input};
    }
    if (!(radius_m > 0.0)) {
        return {{}, geo::MathError::numerical_domain_error};
    }

    const auto beta = geo::authalic_latitude(geodetic_latitude_rad);
    if (!beta.ok()) {
        return {{}, beta.error};
    }

    const geo::Vec3 world = geo::lonlat_to_unit_vector(longitude_rad, beta.value);
    const geo::Vec3 rotated = geo::apply(philbrick_world_to_projection_matrix(), world);
    const geo::LonLatResult framed = geo::unit_vector_to_lonlat(rotated);
    if (!usable_lonlat_result(framed)) {
        return {{}, framed.error};
    }

    return sinu_mollweide_forward(
        framed.value.longitude_rad,
        framed.value.latitude_rad,
        radius_m
    );
}

SphericalResult philbrick_sinu_mollweide_inverse_wgs84(
    const double x,
    const double y,
    const double radius_m
) noexcept {
    const SphericalResult framed = sinu_mollweide_inverse(x, y, radius_m);
    if (!usable_spherical_result(framed)) {
        return framed;
    }

    const geo::Vec3 rotated = geo::lonlat_to_unit_vector(
        framed.value.longitude_rad,
        framed.value.latitude_rad
    );
    const geo::Vec3 world = geo::apply(
        geo::transpose(philbrick_world_to_projection_matrix()),
        rotated
    );
    const geo::LonLatResult authalic = geo::unit_vector_to_lonlat(world);
    if (!usable_lonlat_result(authalic)) {
        return {{}, authalic.error};
    }

    const auto geodetic = geo::geodetic_latitude_from_authalic(
        authalic.value.latitude_rad
    );
    if (!geodetic.ok()) {
        return {{}, geodetic.error};
    }

    const geo::MathError result_error = authalic.error == geo::MathError::indeterminate_coordinate
        ? geo::MathError::indeterminate_coordinate
        : geo::MathError::none;
    return {{authalic.value.longitude_rad, geodetic.value}, result_error};
}

}  // namespace aeris::projection
