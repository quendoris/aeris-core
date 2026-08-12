// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "aeris/projection/primitives.hpp"

namespace aeris::projection {

enum class EqualAreaPrimitive {
    sinusoidal = 0,
    mollweide,
};

[[nodiscard]] PlanarResult project_wgs84_primitive(
    double longitude_rad,
    double geodetic_latitude_rad,
    EqualAreaPrimitive primitive,
    double central_meridian_rad = 0.0,
    double radius_m = geo::authalic_radius_m()
) noexcept;

}  // namespace aeris::projection
