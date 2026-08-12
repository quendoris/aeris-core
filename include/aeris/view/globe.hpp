// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "aeris/geo/rotation.hpp"
#include "aeris/geo/wgs84.hpp"

namespace aeris::view {

// Reference orthographic globe convention:
// - camera looks toward the origin along the +X view axis;
// - screen-right is +Y;
// - screen-up is +Z;
// - positive depth is on the visible hemisphere.
struct GlobePoint final {
    double x = 0.0;
    double y = 0.0;
    double depth = 0.0;
    bool visible = false;
};

struct GlobeResult final {
    GlobePoint value{};
    geo::MathError error = geo::MathError::none;

    [[nodiscard]] constexpr bool ok() const noexcept {
        return error == geo::MathError::none;
    }
};

[[nodiscard]] GlobeResult orthographic_globe_point(
    double authalic_longitude_rad,
    double authalic_latitude_rad,
    const geo::Mat3& world_to_view,
    double radius_m = geo::authalic_radius_m()
) noexcept;

}  // namespace aeris::view
