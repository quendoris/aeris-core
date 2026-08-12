// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstddef>
#include <vector>

namespace aeris::geometry {

struct PlanarPoint final {
    double x = 0.0;
    double y = 0.0;
};

[[nodiscard]] double signed_planar_area(
    const PlanarPoint* points,
    std::size_t count
) noexcept;

[[nodiscard]] inline double signed_planar_area(
    const std::vector<PlanarPoint>& points
) noexcept {
    return signed_planar_area(points.data(), points.size());
}

}  // namespace aeris::geometry
