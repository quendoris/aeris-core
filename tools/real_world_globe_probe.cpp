// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/geo/rotation.hpp"
#include "aeris/geo/wgs84.hpp"
#include "aeris/source/shapefile.hpp"
#include "aeris/view/globe_curve.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

struct Summary final {
    std::size_t records = 0U;
    std::size_t rings = 0U;
    std::size_t source_vertices = 0U;
    std::size_t visible_rings = 0U;
    std::size_t visible_parts = 0U;
    std::size_t projected_vertices = 0U;
    std::size_t horizon_crossings = 0U;
    unsigned deepest_subdivision_level = 0U;
};

[[nodiscard]] double radians(const double degrees) noexcept {
    return degrees * aeris::geo::kPi / 180.0;
}

[[nodiscard]] bool same_point(
    const aeris::geometry::PlanarPoint left,
    const aeris::geometry::PlanarPoint right,
    const double tolerance_m
) noexcept {
    return std::hypot(left.x - right.x, left.y - right.y) <= tolerance_m;
}

[[nodiscard]] bool verify_fragment_geometry(
    const aeris::view::GlobeCurveResult& projected,
    const aeris::view::GlobeCurveOptions& options,
    const double radius_m,
    const std::uint32_t record_number,
    const std::size_t ring_index
) {
    const double radial_tolerance_m = std::max(
        1.0,
        8.0 * options.horizon_tolerance_m
    );

    for (const auto& part : projected.visible_parts) {
        if (part.size() < 2U) {
            std::cerr
                << "degenerate visible globe part: record=" << record_number
                << " ring=" << ring_index << '\n';
            return false;
        }
        for (const auto point : part) {
            const double radial = std::hypot(point.x, point.y);
            if (!std::isfinite(radial) || radial > radius_m + radial_tolerance_m) {
                std::cerr
                    << std::setprecision(17)
                    << "globe point escaped limb: record=" << record_number
                    << " ring=" << ring_index
                    << " radial_m=" << radial
                    << " radius_m=" << radius_m << '\n';
                return false;
            }
        }
    }

    if ((projected.horizon_crossings % 2U) != 0U) {
        std::cerr
            << "closed ring produced odd horizon crossing count: record="
            << record_number << " ring=" << ring_index
            << " crossings=" << projected.horizon_crossings << '\n';
        return false;
    }

    if (projected.horizon_crossings == 0U) {
        if (projected.visible_parts.empty()) {
            return true;
        }
        if (projected.visible_parts.size() != 1U) {
            std::cerr
                << "zero-crossing ring produced multiple visible parts: record="
                << record_number << " ring=" << ring_index << '\n';
            return false;
        }
        const auto& part = projected.visible_parts.front();
        if (!same_point(part.front(), part.back(), radial_tolerance_m)) {
            std::cerr
                << "fully visible zero-crossing ring is not closed: record="
                << record_number << " ring=" << ring_index << '\n';
            return false;
        }
        return true;
    }

    for (const auto& part : projected.visible_parts) {
        const double first_radius = std::hypot(part.front().x, part.front().y);
        const double last_radius = std::hypot(part.back().x, part.back().y);
        if (std::abs(first_radius - radius_m) > radial_tolerance_m ||
            std::abs(last_radius - radius_m) > radial_tolerance_m) {
            std::cerr
                << std::setprecision(17)
                << "partial globe fragment does not terminate on limb: record="
                << record_number << " ring=" << ring_index
                << " first_radius_m=" << first_radius
                << " last_radius_m=" << last_radius
                << " radius_m=" << radius_m << '\n';
            return false;
        }
    }

    return true;
}

[[nodiscard]] bool write_svg(
    const std::filesystem::path& output_path,
    const std::vector<std::vector<aeris::geometry::PlanarPoint>>& visible_parts,
    const Summary& summary,
    const double radius_m,
    const double center_longitude_deg,
    const double center_latitude_deg
) {
    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    if (!output) {
        std::cerr << "unable to open globe SVG output: " << output_path << '\n';
        return false;
    }

    constexpr double center_x = 450.0;
    constexpr double center_y = 430.0;
    constexpr double screen_radius = 360.0;
    const double pixels_per_metre = screen_radius / radius_m;

    output
        << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"900\" height=\"900\" "
           "viewBox=\"0 0 900 900\" role=\"img\" "
           "aria-label=\"AERIS Natural Earth orthographic authalic globe proof\">\n"
        << "<style>"
           ".background{fill:#f5f5f3;}"
           ".ocean{fill:#ecebe7;stroke:#111;stroke-width:2;}"
           ".coast{fill:none;stroke:#222;stroke-width:.7;stroke-linecap:round;stroke-linejoin:round;}"
           ".title{font:24px sans-serif;fill:#111;}"
           ".meta{font:14px monospace;fill:#444;}"
           "</style>\n"
        << "<rect class=\"background\" width=\"900\" height=\"900\"/>\n"
        << "<circle class=\"ocean\" cx=\"" << center_x
        << "\" cy=\"" << center_y
        << "\" r=\"" << screen_radius << "\"/>\n";

    output << std::fixed << std::setprecision(3);
    for (const auto& part : visible_parts) {
        if (part.size() < 2U) {
            continue;
        }
        const auto screen_x = [&](const double x) {
            return center_x + x * pixels_per_metre;
        };
        const auto screen_y = [&](const double y) {
            return center_y - y * pixels_per_metre;
        };

        output << "<path class=\"coast\" d=\"M"
               << screen_x(part.front().x) << ','
               << screen_y(part.front().y) << ' ';
        for (std::size_t index = 1U; index < part.size(); ++index) {
            output << 'L' << screen_x(part[index].x) << ','
                   << screen_y(part[index].y) << ' ';
        }
        output << "\"/>\n";
    }

    output
        << "<circle cx=\"" << center_x
        << "\" cy=\"" << center_y
        << "\" r=\"3\" fill=\"#111\"/>\n"
        << "<text class=\"title\" x=\"450\" y=\"42\" text-anchor=\"middle\">"
           "Natural Earth / Authalic Orthographic Globe</text>\n"
        << "<text class=\"meta\" x=\"40\" y=\"825\">camera center: "
        << center_longitude_deg << "° lon, " << center_latitude_deg
        << "° geodetic lat</text>\n"
        << "<text class=\"meta\" x=\"40\" y=\"850\">records: "
        << summary.records << " / rings: " << summary.rings
        << " / visible parts: " << summary.visible_parts
        << " / horizon crossings: " << summary.horizon_crossings << "</text>\n"
        << "<text class=\"meta\" x=\"40\" y=\"875\">source vertices: "
        << summary.source_vertices << " / projected vertices: "
        << summary.projected_vertices << " / max subdivision level: "
        << summary.deepest_subdivision_level << "</text>\n"
        << "</svg>\n";

    output.flush();
    return static_cast<bool>(output);
}

}  // namespace

int main(const int argc, char** const argv) {
    if (argc != 3) {
        std::cerr
            << "usage: aeris_real_world_globe_probe <file.shp> <output.svg>\n";
        return EXIT_FAILURE;
    }

    const std::filesystem::path source_path(argv[1]);
    const std::filesystem::path output_path(argv[2]);

    const auto source = aeris::source::read_polygon_shapefile(source_path);
    if (!source.ok()) {
        std::cerr
            << "Shapefile parse failed: error=" << static_cast<int>(source.error)
            << " record=" << source.failed_record_number
            << " geographic_error=" << static_cast<int>(source.geographic_error)
            << " diagnostic=" << source.diagnostic << '\n';
        return EXIT_FAILURE;
    }

    constexpr double center_longitude_deg = 15.0;
    constexpr double center_latitude_deg = 20.0;
    const auto center_beta = aeris::geo::authalic_latitude(
        radians(center_latitude_deg)
    );
    if (!center_beta.ok()) {
        std::cerr << "unable to derive authalic camera latitude\n";
        return EXIT_FAILURE;
    }

    const aeris::geo::Mat3 world_to_view = aeris::geo::multiply(
        aeris::geo::rotation_y(center_beta.value),
        aeris::geo::rotation_z(-radians(center_longitude_deg))
    );
    if (!aeris::geo::is_rotation_matrix(world_to_view)) {
        std::cerr << "derived globe camera is not a valid rotation\n";
        return EXIT_FAILURE;
    }

    const double radius_m = aeris::geo::authalic_radius_m();
    aeris::view::GlobeCurveOptions options{};
    options.geometric_tolerance_m = 5'000.0;
    options.horizon_tolerance_m = 0.01;
    options.max_subdivision_depth = 28U;
    options.max_root_iterations = 80U;
    options.max_segments = 1'000'000U;

    Summary summary{};
    summary.records = source.records.size();
    std::vector<std::vector<aeris::geometry::PlanarPoint>> visible_parts;

    for (const auto& record : source.records) {
        for (std::size_t ring_index = 0U;
             ring_index < record.rings.size();
             ++ring_index) {
            const auto& ring = record.rings[ring_index].geometry;
            ++summary.rings;
            summary.source_vertices += ring.vertices.size();

            const auto projected = aeris::view::project_visible_wgs84_linear_ring(
                ring,
                world_to_view,
                options,
                radius_m
            );
            if (!projected.ok()) {
                std::cerr
                    << "globe projection failure: record=" << record.record_number
                    << " ring=" << ring_index
                    << " error=" << static_cast<int>(projected.error)
                    << " sample_error=" << static_cast<int>(projected.sample_error)
                    << " crossings=" << projected.horizon_crossings
                    << " depth=" << projected.deepest_subdivision_level
                    << " vertices=" << projected.projected_vertices
                    << '\n';
                return EXIT_FAILURE;
            }

            if (!verify_fragment_geometry(
                    projected,
                    options,
                    radius_m,
                    record.record_number,
                    ring_index
                )) {
                return EXIT_FAILURE;
            }

            if (!projected.visible_parts.empty()) {
                ++summary.visible_rings;
            }
            summary.visible_parts += projected.visible_parts.size();
            summary.projected_vertices += projected.projected_vertices;
            summary.horizon_crossings += projected.horizon_crossings;
            summary.deepest_subdivision_level = std::max(
                summary.deepest_subdivision_level,
                projected.deepest_subdivision_level
            );

            for (const auto& part : projected.visible_parts) {
                visible_parts.push_back(part);
            }
        }
    }

    if (summary.records != 127U ||
        summary.rings != 128U ||
        summary.source_vertices != 5015U) {
        std::cerr
            << "unexpected pinned Natural Earth geometry cardinality: records="
            << summary.records << " rings=" << summary.rings
            << " source_vertices=" << summary.source_vertices << '\n';
        return EXIT_FAILURE;
    }
    if (summary.visible_rings == 0U ||
        summary.visible_parts == 0U ||
        summary.horizon_crossings == 0U) {
        std::cerr
            << "real-world globe proof failed to exercise visibility topology: "
            << "visible_rings=" << summary.visible_rings
            << " visible_parts=" << summary.visible_parts
            << " horizon_crossings=" << summary.horizon_crossings << '\n';
        return EXIT_FAILURE;
    }

    if (!write_svg(
            output_path,
            visible_parts,
            summary,
            radius_m,
            center_longitude_deg,
            center_latitude_deg
        )) {
        std::cerr << "unable to write globe SVG\n";
        return EXIT_FAILURE;
    }

    std::cout
        << "real_world_globe_probe: PASS\n"
        << "records=" << summary.records << '\n'
        << "rings=" << summary.rings << '\n'
        << "source_vertices=" << summary.source_vertices << '\n'
        << "normalized_boundary_coordinates="
        << source.normalized_boundary_coordinates << '\n'
        << "visible_rings=" << summary.visible_rings << '\n'
        << "visible_parts=" << summary.visible_parts << '\n'
        << "horizon_crossings=" << summary.horizon_crossings << '\n'
        << "projected_vertices=" << summary.projected_vertices << '\n'
        << "deepest_subdivision_level="
        << summary.deepest_subdivision_level << '\n'
        << "camera_center_longitude_deg=" << center_longitude_deg << '\n'
        << "camera_center_geodetic_latitude_deg=" << center_latitude_deg << '\n'
        << "svg=" << output_path.string() << '\n';

    return EXIT_SUCCESS;
}
