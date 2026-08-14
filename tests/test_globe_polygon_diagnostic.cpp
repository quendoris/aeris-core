// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/view/globe_polygon.hpp"

#include "aeris/geo/wgs84.hpp"

#include <algorithm>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <vector>

namespace {

[[nodiscard]] double radians(const double degrees) {
    return degrees * aeris::geo::kPi / 180.0;
}

[[nodiscard]] aeris::view::GlobePolygonOptions options() {
    aeris::view::GlobePolygonOptions value{};
    value.curve.geometric_tolerance_m = 1e-4;
    value.curve.horizon_tolerance_m = 1e-10;
    value.curve.max_subdivision_depth = 32U;
    value.curve.max_root_iterations = 80U;
    value.curve.max_segments = 100'000U;
    value.horizon_arc_tolerance_m = 1e-4;
    value.max_horizon_arc_segments = 100'000U;
    value.max_output_rings = 64U;
    return value;
}

[[nodiscard]] aeris::geometry::LinearRing ring(
    const std::vector<aeris::geometry::GeodeticPoint>& points,
    const aeris::geometry::RingInteriorSide side
) {
    auto result = aeris::geometry::canonicalize_wgs84_linear_ring(points);
    if (!result.ok()) {
        return {};
    }
    result.value.interior_side = side;
    return result.value;
}

void dump(
    const char* const name,
    const aeris::view::GlobePolygonResult& result
) {
    std::cerr
        << std::setprecision(17)
        << "DIAG " << name
        << " ok=" << result.ok()
        << " error=" << static_cast<int>(result.error)
        << " geographic_error=" << static_cast<int>(result.geographic_error)
        << " curve_error=" << static_cast<int>(result.curve_error)
        << " sample_error=" << static_cast<int>(result.sample_error)
        << " source_m2=" << result.source_signed_area_m2
        << " planar_m2=" << result.planar_signed_area_m2
        << " disk_m2=" << result.visible_disk_area_m2
        << " rings=" << result.rings.size()
        << " crossings=" << result.horizon_crossings
        << " arc_segments=" << result.horizon_arc_segments
        << " vertices=" << result.projected_vertices
        << '\n';
}

}  // namespace

int main() {
    constexpr double radius = 10.0;

    const auto visible_major = ring(
        {
            {radians(-30.0), radians(-20.0)},
            {radians(30.0), radians(-20.0)},
            {radians(30.0), radians(20.0)},
            {radians(-30.0), radians(20.0)},
        },
        aeris::geometry::RingInteriorSide::right
    );
    dump(
        "visible_major",
        aeris::view::project_visible_wgs84_linear_polygon_ring(
            visible_major,
            aeris::geo::Mat3{},
            options(),
            radius
        )
    );

    const auto partial_left = ring(
        {
            {radians(60.0), radians(-20.0)},
            {radians(120.0), radians(-20.0)},
            {radians(120.0), radians(20.0)},
            {radians(60.0), radians(20.0)},
        },
        aeris::geometry::RingInteriorSide::left
    );
    dump(
        "partial_left",
        aeris::view::project_visible_wgs84_linear_polygon_ring(
            partial_left,
            aeris::geo::Mat3{},
            options(),
            radius
        )
    );

    auto reversed_points = std::vector<aeris::geometry::GeodeticPoint>{
        {radians(60.0), radians(-20.0)},
        {radians(120.0), radians(-20.0)},
        {radians(120.0), radians(20.0)},
        {radians(60.0), radians(20.0)},
    };
    std::reverse(reversed_points.begin(), reversed_points.end());
    const auto partial_right = ring(
        reversed_points,
        aeris::geometry::RingInteriorSide::right
    );
    dump(
        "partial_right",
        aeris::view::project_visible_wgs84_linear_polygon_ring(
            partial_right,
            aeris::geo::Mat3{},
            options(),
            radius
        )
    );

    const auto four_crossings = ring(
        {
            {radians(120.0), radians(50.0)},
            {radians(60.0), radians(50.0)},
            {radians(60.0), radians(20.0)},
            {radians(100.0), radians(20.0)},
            {radians(100.0), radians(-20.0)},
            {radians(60.0), radians(-20.0)},
            {radians(60.0), radians(-50.0)},
            {radians(120.0), radians(-50.0)},
        },
        aeris::geometry::RingInteriorSide::left
    );
    dump(
        "four_crossings",
        aeris::view::project_visible_wgs84_linear_polygon_ring(
            four_crossings,
            aeris::geo::Mat3{},
            options(),
            radius
        )
    );

    // Deliberately fail so CTest prints the diagnostic stream on every runner.
    return EXIT_FAILURE;
}
