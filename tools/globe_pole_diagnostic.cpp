// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/geo/rotation.hpp"
#include "aeris/geo/wgs84.hpp"
#include "aeris/geometry/planar.hpp"
#include "aeris/source/shapefile.hpp"
#include "aeris/view/globe_curve.hpp"
#include "aeris/view/globe_polygon.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string_view>

namespace {

[[nodiscard]] double radians(const double degrees) noexcept {
    return degrees * aeris::geo::kPi / 180.0;
}

[[nodiscard]] double degrees(const double radians_value) noexcept {
    return radians_value * 180.0 / aeris::geo::kPi;
}

[[nodiscard]] bool parse_record(
    const std::string_view text,
    unsigned& value
) noexcept {
    const auto parsed = std::from_chars(
        text.data(),
        text.data() + text.size(),
        value
    );
    return parsed.ec == std::errc{} &&
           parsed.ptr == text.data() + text.size();
}

[[nodiscard]] double angle(const aeris::geometry::PlanarPoint point) noexcept {
    double value = std::atan2(point.y, point.x);
    if (value < 0.0) {
        value += 2.0 * aeris::geo::kPi;
    }
    return value;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: aeris_globe_pole_diagnostic <shp> <record>\n";
        return EXIT_FAILURE;
    }

    unsigned record_number = 0U;
    if (!parse_record(argv[2], record_number)) {
        std::cerr << "invalid record number\n";
        return EXIT_FAILURE;
    }

    const auto parsed = aeris::source::read_polygon_shapefile(argv[1]);
    if (!parsed.ok()) {
        std::cerr << "shapefile read failed: error="
                  << static_cast<int>(parsed.error)
                  << " record=" << parsed.failed_record_number
                  << " diagnostic=" << parsed.diagnostic << '\n';
        return EXIT_FAILURE;
    }

    const aeris::source::ShapefileRecord* selected = nullptr;
    for (const auto& record : parsed.records) {
        if (record.record_number == record_number) {
            selected = &record;
            break;
        }
    }
    if (selected == nullptr || selected->rings.empty()) {
        std::cerr << "record/ring not found\n";
        return EXIT_FAILURE;
    }

    auto ring = selected->rings.front().geometry;
    ring.interior_side = selected->rings.front().role == aeris::source::RingRole::exterior
        ? aeris::geometry::RingInteriorSide::right
        : aeris::geometry::RingInteriorSide::left;

    std::cout << std::setprecision(17)
              << "record=" << record_number
              << " vertices=" << ring.vertices.size()
              << " winding=" << ring.longitude_winding
              << " side=" << static_cast<int>(ring.interior_side) << '\n';

    std::size_t pole_vertices = 0U;
    for (std::size_t index = 0U; index < ring.vertices.size(); ++index) {
        const auto current = ring.vertices[index];
        if (std::abs(std::abs(current.latitude_rad) - aeris::geo::kHalfPi) > 1e-14) {
            continue;
        }
        ++pole_vertices;
        const std::size_t previous_index =
            index == 0U ? ring.vertices.size() - 1U : index - 1U;
        const std::size_t next_index = (index + 1U) % ring.vertices.size();
        const auto previous = ring.vertices[previous_index];
        const auto next = ring.vertices[next_index];
        std::cout
            << "pole index=" << index
            << " lon_deg=" << degrees(current.longitude_rad)
            << " lat_deg=" << degrees(current.latitude_rad)
            << " prev_lon_deg=" << degrees(previous.longitude_rad)
            << " prev_lat_deg=" << degrees(previous.latitude_rad)
            << " next_lon_deg=" << degrees(next.longitude_rad)
            << " next_lat_deg=" << degrees(next.latitude_rad)
            << '\n';
    }
    std::cout << "pole_vertices=" << pole_vertices << '\n';

    const auto beta = aeris::geo::authalic_latitude(radians(20.0));
    if (!beta.ok()) {
        std::cerr << "camera beta failed\n";
        return EXIT_FAILURE;
    }
    const aeris::geo::Mat3 world_to_view = aeris::geo::multiply(
        aeris::geo::rotation_y(beta.value),
        aeris::geo::rotation_z(-radians(15.0))
    );
    const double radius = aeris::geo::authalic_radius_m();

    aeris::view::GlobeCurveOptions curve_options{};
    curve_options.geometric_tolerance_m = 5'000.0;
    curve_options.horizon_tolerance_m = 0.01;
    curve_options.max_subdivision_depth = 32U;
    curve_options.max_root_iterations = 80U;
    curve_options.max_segments = 5'000'000U;

    const auto curve = aeris::view::project_visible_wgs84_linear_ring(
        ring,
        world_to_view,
        curve_options,
        radius
    );
    std::cout
        << "curve_error=" << static_cast<int>(curve.error)
        << " crossings=" << curve.horizon_crossings
        << " parts=" << curve.visible_parts.size()
        << " vertices=" << curve.projected_vertices << '\n';
    for (std::size_t index = 0U; index < curve.visible_parts.size(); ++index) {
        const auto& part = curve.visible_parts[index];
        if (part.empty()) {
            continue;
        }
        std::cout
            << "part=" << index
            << " size=" << part.size()
            << " start_angle_deg=" << degrees(angle(part.front()))
            << " end_angle_deg=" << degrees(angle(part.back()))
            << '\n';
    }

    struct Refinement final {
        double curve_m;
        double arc_m;
    };
    constexpr std::array<Refinement, 8U> refinements{{
        {5'000.0, 500.0},
        {2'500.0, 250.0},
        {1'000.0, 100.0},
        {500.0, 50.0},
        {100.0, 10.0},
        {50.0, 5.0},
        {25.0, 2.5},
        {10.0, 1.0},
    }};

    for (const Refinement refinement : refinements) {
        aeris::view::GlobePolygonOptions polygon_options{};
        polygon_options.curve = curve_options;
        polygon_options.curve.geometric_tolerance_m = refinement.curve_m;
        polygon_options.horizon_arc_tolerance_m = refinement.arc_m;
        polygon_options.max_horizon_arc_segments = 5'000'000U;
        polygon_options.max_output_rings = 4096U;

        const auto polygon = aeris::view::project_visible_wgs84_linear_polygon_ring(
            ring,
            world_to_view,
            polygon_options,
            radius
        );
        std::cout
            << "refine curve_m=" << refinement.curve_m
            << " arc_m=" << refinement.arc_m
            << " error=" << static_cast<int>(polygon.error)
            << " output_rings=" << polygon.rings.size()
            << " signed_area=" << polygon.planar_signed_area_m2
            << " crossings=" << polygon.horizon_crossings
            << " arc_segments=" << polygon.horizon_arc_segments
            << " vertices=" << polygon.projected_vertices
            << '\n';
        for (std::size_t index = 0U; index < polygon.rings.size(); ++index) {
            std::cout
                << "  ring=" << index
                << " size=" << polygon.rings[index].size()
                << " signed_area="
                << aeris::geometry::signed_planar_area(polygon.rings[index])
                << '\n';
        }
    }

    return EXIT_SUCCESS;
}
