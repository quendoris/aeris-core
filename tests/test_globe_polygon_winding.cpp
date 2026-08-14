// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/view/globe_polygon.hpp"

#include "aeris/geo/wgs84.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

[[nodiscard]] double radians(const double degrees) {
    return degrees * aeris::geo::kPi / 180.0;
}

}  // namespace

int main() {
    const std::vector<aeris::geometry::GeodeticPoint> points{
        {radians(-180.0), radians(-70.0)},
        {radians(-120.0), radians(-70.0)},
        {radians(-60.0), radians(-70.0)},
        {radians(0.0), radians(-70.0)},
        {radians(60.0), radians(-70.0)},
        {radians(120.0), radians(-70.0)},
    };

    auto canonical = aeris::geometry::canonicalize_wgs84_linear_ring(points);
    if (!canonical.ok()) {
        std::cerr << "polar winding fixture failed to canonicalize\n";
        return EXIT_FAILURE;
    }
    if (canonical.value.longitude_winding != 1) {
        std::cerr << "polar winding fixture did not produce winding +1\n";
        return EXIT_FAILURE;
    }
    canonical.value.interior_side = aeris::geometry::RingInteriorSide::right;

    aeris::view::GlobePolygonOptions options{};
    options.curve.geometric_tolerance_m = 1e-4;
    options.curve.horizon_tolerance_m = 1e-10;
    options.curve.max_subdivision_depth = 32U;
    options.curve.max_root_iterations = 80U;
    options.curve.max_segments = 100'000U;
    options.horizon_arc_tolerance_m = 1e-4;
    options.max_horizon_arc_segments = 100'000U;
    options.max_output_rings = 64U;

    const auto result = aeris::view::project_visible_wgs84_linear_polygon_ring(
        canonical.value,
        aeris::geo::Mat3{},
        options,
        10.0
    );
    if (!result.ok()) {
        std::cerr
            << "polar winding projection failed: error="
            << static_cast<int>(result.error)
            << " crossings=" << result.horizon_crossings
            << " rings=" << result.rings.size()
            << " area=" << result.planar_signed_area_m2 << '\n';
        return EXIT_FAILURE;
    }
    if (result.horizon_crossings != 2U || result.rings.size() != 1U) {
        std::cerr
            << "unexpected polar winding topology: crossings="
            << result.horizon_crossings
            << " rings=" << result.rings.size() << '\n';
        return EXIT_FAILURE;
    }
    if (!(result.planar_signed_area_m2 < 0.0)) {
        std::cerr
            << "polar winding right-side region lost orientation: area="
            << result.planar_signed_area_m2 << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "globe_polygon_winding: PASS\n";
    return EXIT_SUCCESS;
}
