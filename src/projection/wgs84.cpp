// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/projection/wgs84.hpp"

#include "aeris/projection/sinu_mollweide.hpp"

#include <cmath>

namespace aeris::projection {

PlanarResult project_wgs84_primitive(
    const double longitude_rad,
    const double geodetic_latitude_rad,
    const EqualAreaPrimitive primitive,
    const double central_meridian_rad,
    const double radius_m
) noexcept {
    if (!std::isfinite(longitude_rad) || !std::isfinite(central_meridian_rad)) {
        return {{}, geo::MathError::non_finite_input};
    }

    const geo::ScalarResult beta = geo::authalic_latitude(geodetic_latitude_rad);
    if (!beta.ok()) {
        return {{}, beta.error};
    }

    const double longitude_delta = longitude_rad - central_meridian_rad;

    switch (primitive) {
        case EqualAreaPrimitive::sinusoidal:
            return sinusoidal_forward(longitude_delta, beta.value, radius_m);
        case EqualAreaPrimitive::mollweide:
            return mollweide_forward(longitude_delta, beta.value, radius_m);
        case EqualAreaPrimitive::sinu_mollweide:
            return sinu_mollweide_forward(longitude_delta, beta.value, radius_m);
    }

    return {{}, geo::MathError::numerical_domain_error};
}

}  // namespace aeris::projection
