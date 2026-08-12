// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/view/globe.hpp"

#include <cmath>

namespace aeris::view {
GlobeResult orthographic_globe_point(
    const double authalic_longitude_rad,
    const double authalic_latitude_rad,
    const geo::Mat3& world_to_view,
    const double radius_m
) noexcept {
    if (!std::isfinite(authalic_longitude_rad) ||
        !std::isfinite(authalic_latitude_rad) ||
        !std::isfinite(radius_m)) {
        return {{}, geo::MathError::non_finite_input};
    }
    if (authalic_latitude_rad < -geo::kHalfPi || authalic_latitude_rad > geo::kHalfPi) {
        return {{}, geo::MathError::latitude_out_of_range};
    }
    if (radius_m <= 0.0 || !geo::is_rotation_matrix(world_to_view)) {
        return {{}, geo::MathError::numerical_domain_error};
    }

    const geo::Vec3 world = geo::lonlat_to_unit_vector(
        authalic_longitude_rad,
        authalic_latitude_rad
    );
    const geo::Vec3 view = geo::apply(world_to_view, world);
    if (!std::isfinite(view.x) || !std::isfinite(view.y) || !std::isfinite(view.z)) {
        return {{}, geo::MathError::numerical_domain_error};
    }

    return {{
                radius_m * view.y,
                radius_m * view.z,
                radius_m * view.x,
                view.x >= 0.0,
            },
            geo::MathError::none};
}

}  // namespace aeris::view
