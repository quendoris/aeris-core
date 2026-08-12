// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/geometry/planar.hpp"

#include <cmath>
#include <limits>

namespace aeris::geometry {

double signed_planar_area(const PlanarPoint* const points, const std::size_t count) noexcept {
    if (points == nullptr || count < 3U) {
        return 0.0;
    }

    const PlanarPoint origin = points[0];
    double sum = 0.0;
    double compensation = 0.0;

    // Triangulating relative to the first vertex avoids the large cancelling
    // coordinate products produced by the textbook shoelace formula when a
    // small polygon is translated far from the planar origin.
    for (std::size_t index = 1U; index + 1U < count; ++index) {
        const double ax = points[index].x - origin.x;
        const double ay = points[index].y - origin.y;
        const double bx = points[index + 1U].x - origin.x;
        const double by = points[index + 1U].y - origin.y;
        const double cross = ax * by - ay * bx;

        if (!std::isfinite(cross)) {
            return std::numeric_limits<double>::quiet_NaN();
        }

        const double corrected = cross - compensation;
        const double next = sum + corrected;
        compensation = (next - sum) - corrected;
        sum = next;
    }

    return 0.5 * sum;
}

}  // namespace aeris::geometry
