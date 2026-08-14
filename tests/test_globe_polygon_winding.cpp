// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/view/globe_polygon.hpp"

#include "aeris/geo/rotation.hpp"
#include "aeris/geo/wgs84.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

[[nodiscard]] double radians(const double degrees) {
    return degrees * aeris::geo::kPi / 180.0;
}

[[nodiscard]] aeris::view::GlobePolygonOptions strict_options() {
    aeris::view::GlobePolygonOptions options{};
    options.curve.geometric_tolerance_m = 1e-4;
    options.curve.horizon_tolerance_m = 1e-10;
    options.curve.max_subdivision_depth = 32U;
    options.curve.max_root_iterations = 80U;
    options.curve.max_segments = 100'000U;
    options.horizon_arc_tolerance_m = 1e-4;
    options.max_horizon_arc_segments = 100'000U;
    options.max_output_rings = 64U;
    return options;
}

[[nodiscard]] aeris::geo::Mat3 real_world_camera() {
    const auto beta = aeris::geo::authalic_latitude(radians(20.0));
    if (!beta.ok()) {
        return {};
    }
    return aeris::geo::multiply(
        aeris::geo::rotation_y(beta.value),
        aeris::geo::rotation_z(-radians(15.0))
    );
}

[[nodiscard]] bool run_case(
    const std::string_view name,
    const std::vector<aeris::geometry::GeodeticPoint>& points,
    const aeris::geo::Mat3& camera,
    const bool require_two_crossings
) {
    auto canonical = aeris::geometry::canonicalize_wgs84_linear_ring(points);
    if (!canonical.ok()) {
        std::cerr << name << ": failed to canonicalize\n";
        return false;
    }
    if (canonical.value.longitude_winding != 1) {
        std::cerr << name << ": expected winding +1, got "
                  << canonical.value.longitude_winding << '\n';
        return false;
    }
    canonical.value.interior_side = aeris::geometry::RingInteriorSide::right;

    const auto result = aeris::view::project_visible_wgs84_linear_polygon_ring(
        canonical.value,
        camera,
        strict_options(),
        10.0
    );
    if (!result.ok()) {
        std::cerr
            << name
            << ": projection failed error=" << static_cast<int>(result.error)
            << " crossings=" << result.horizon_crossings
            << " rings=" << result.rings.size()
            << " area=" << result.planar_signed_area_m2 << '\n';
        return false;
    }
    if ((result.horizon_crossings % 2U) != 0U) {
        std::cerr << name << ": odd horizon crossing count\n";
        return false;
    }
    if (require_two_crossings &&
        (result.horizon_crossings != 2U || result.rings.size() != 1U)) {
        std::cerr
            << name << ": unexpected topology crossings="
            << result.horizon_crossings
            << " rings=" << result.rings.size() << '\n';
        return false;
    }
    if (result.rings.empty() || !(result.planar_signed_area_m2 < 0.0)) {
        std::cerr
            << name << ": right-side polar region lost orientation area="
            << result.planar_signed_area_m2
            << " crossings=" << result.horizon_crossings
            << " rings=" << result.rings.size() << '\n';
        return false;
    }
    return true;
}

}  // namespace

int main() {
    const std::vector<aeris::geometry::GeodeticPoint> smooth_cap{
        {radians(-180.0), radians(-70.0)},
        {radians(-120.0), radians(-70.0)},
        {radians(-60.0), radians(-70.0)},
        {radians(0.0), radians(-70.0)},
        {radians(60.0), radians(-70.0)},
        {radians(120.0), radians(-70.0)},
    };
    if (!run_case(
            "smooth polar winding",
            smooth_cap,
            aeris::geo::Mat3{},
            true
        )) {
        return EXIT_FAILURE;
    }

    const aeris::geo::Mat3 camera = real_world_camera();
    if (!aeris::geo::is_rotation_matrix(camera)) {
        std::cerr << "unable to construct real-world test camera\n";
        return EXIT_FAILURE;
    }

    const std::vector<aeris::geometry::GeodeticPoint> single_pole_touch{
        {radians(-180.0), radians(-70.0)},
        {radians(-120.0), radians(-70.0)},
        {radians(-60.0), radians(-70.0)},
        {radians(0.0), radians(-90.0)},
        {radians(60.0), radians(-70.0)},
        {radians(120.0), radians(-70.0)},
    };
    if (!run_case(
            "single exact-pole touch",
            single_pole_touch,
            camera,
            false
        )) {
        return EXIT_FAILURE;
    }

    const std::vector<aeris::geometry::GeodeticPoint> repeated_pole_run{
        {radians(-180.0), radians(-70.0)},
        {radians(-120.0), radians(-70.0)},
        {radians(-60.0), radians(-90.0)},
        {radians(0.0), radians(-90.0)},
        {radians(60.0), radians(-90.0)},
        {radians(120.0), radians(-70.0)},
    };
    if (!run_case(
            "repeated exact-pole longitudes",
            repeated_pole_run,
            camera,
            false
        )) {
        return EXIT_FAILURE;
    }

    std::cout << "globe_polygon_winding: PASS\n";
    return EXIT_SUCCESS;
}
