// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/projection/ring.hpp"
#include "aeris/source/shapefile.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string_view>

namespace {

[[nodiscard]] bool parse_u32(const std::string_view text, std::uint32_t& value) noexcept {
    if (text.empty()) return false;
    const char* const first = text.data();
    const char* const last = text.data() + text.size();
    const auto parsed = std::from_chars(first, last, value);
    return parsed.ec == std::errc{} && parsed.ptr == last;
}

[[nodiscard]] bool parse_size(const std::string_view text, std::size_t& value) noexcept {
    if (text.empty()) return false;
    const char* const first = text.data();
    const char* const last = text.data() + text.size();
    const auto parsed = std::from_chars(first, last, value);
    return parsed.ec == std::errc{} && parsed.ptr == last;
}

[[nodiscard]] const aeris::source::ShapefileRecord* find_record(
    const aeris::source::ShapefilePolygonResult& source,
    const std::uint32_t record_number
) noexcept {
    for (const auto& record : source.records) {
        if (record.record_number == record_number) return &record;
    }
    return nullptr;
}

void print_ring_geometry(const aeris::source::ShapefileRing& source_ring) {
    const auto& ring = source_ring.geometry;
    double minimum_longitude = ring.vertices.front().longitude_rad;
    double maximum_longitude = minimum_longitude;
    double minimum_latitude = ring.vertices.front().latitude_rad;
    double maximum_latitude = minimum_latitude;

    for (const auto point : ring.vertices) {
        minimum_longitude = std::min(minimum_longitude, point.longitude_rad);
        maximum_longitude = std::max(maximum_longitude, point.longitude_rad);
        minimum_latitude = std::min(minimum_latitude, point.latitude_rad);
        maximum_latitude = std::max(maximum_latitude, point.latitude_rad);
    }
    minimum_longitude = std::min(minimum_longitude, ring.closing_longitude_rad);
    maximum_longitude = std::max(maximum_longitude, ring.closing_longitude_rad);

    constexpr double rad_to_deg = 180.0 / aeris::geo::kPi;
    std::cout
        << std::setprecision(17)
        << "canonical_geometry:"
        << " first_lon_deg=" << ring.vertices.front().longitude_rad * rad_to_deg
        << " first_lat_deg=" << ring.vertices.front().latitude_rad * rad_to_deg
        << " last_lon_deg=" << ring.vertices.back().longitude_rad * rad_to_deg
        << " last_lat_deg=" << ring.vertices.back().latitude_rad * rad_to_deg
        << " closing_lon_deg=" << ring.closing_longitude_rad * rad_to_deg
        << " closing_lat_deg=" << ring.vertices.front().latitude_rad * rad_to_deg
        << " min_lon_deg=" << minimum_longitude * rad_to_deg
        << " max_lon_deg=" << maximum_longitude * rad_to_deg
        << " min_lat_deg=" << minimum_latitude * rad_to_deg
        << " max_lat_deg=" << maximum_latitude * rad_to_deg
        << " span_lon_deg=" << (maximum_longitude - minimum_longitude) * rad_to_deg
        << '\n';
}

void print_seam_crossings(
    const aeris::geometry::LinearRing& ring,
    const double central_meridian_rad
) {
    constexpr double two_pi = 2.0 * aeris::geo::kPi;
    constexpr double rad_to_deg = 180.0 / aeris::geo::kPi;
    std::size_t crossing_count = 0U;

    for (std::size_t index = 0U; index < ring.vertices.size(); ++index) {
        const auto start = ring.vertices[index];
        const aeris::geometry::GeodeticPoint end =
            index + 1U < ring.vertices.size()
                ? ring.vertices[index + 1U]
                : aeris::geometry::GeodeticPoint{
                      ring.closing_longitude_rad,
                      ring.vertices.front().latitude_rad,
                  };
        const double delta = end.longitude_rad - start.longitude_rad;
        if (delta == 0.0) continue;

        const double low = std::min(start.longitude_rad, end.longitude_rad);
        const double high = std::max(start.longitude_rad, end.longitude_rad);
        const double base_seam = central_meridian_rad + aeris::geo::kPi;
        const long long first_k = static_cast<long long>(
            std::ceil((low - base_seam) / two_pi)
        );
        const long long last_k = static_cast<long long>(
            std::floor((high - base_seam) / two_pi)
        );

        for (long long k = first_k; k <= last_k; ++k) {
            const double seam = base_seam + static_cast<double>(k) * two_pi;
            const double parameter = (seam - start.longitude_rad) / delta;
            if (!(parameter > 0.0 && parameter <= 1.0)) continue;

            const auto crossing = aeris::geometry::interpolate_wgs84_linear_edge(
                start,
                end,
                parameter
            );
            ++crossing_count;
            std::cout
                << std::setprecision(17)
                << "seam_crossing: index=" << crossing_count
                << " edge=" << index
                << " t=" << parameter
                << " seam_lon_deg=" << seam * rad_to_deg
                << " latitude_deg=" << crossing.latitude_rad * rad_to_deg
                << " direction=" << (delta > 0.0 ? "+" : "-")
                << '\n';
        }
    }

    std::cout << "seam_crossing_count=" << crossing_count << '\n';
}

void probe_adapter(
    const aeris::geometry::LinearRing& ring,
    const aeris::projection::ProjectionAdapter& adapter
) {
    aeris::projection::RingProjectionOptions options{};
    options.adapter = &adapter;
    options.relative_area_tolerance = 1e-7;
    options.absolute_area_tolerance_m2 = 10'000.0;
    options.initial_geometric_tolerance_m = 500.0;
    options.initial_local_area_tolerance_m2 = 100'000'000.0;
    options.subdivision_max_depth = 30U;
    options.subdivision_max_segments_per_edge = 262'144U;

    std::cout
        << "projection=" << adapter.descriptor().model_id
        << " name=\"" << adapter.descriptor().display_name << "\"\n";

    for (unsigned round_limit = 1U; round_limit <= 18U; ++round_limit) {
        options.max_refinement_rounds = round_limit;
        const auto result = aeris::projection::project_wgs84_linear_ring_verified(ring, options);

        std::cout
            << std::setprecision(17)
            << "  limit=" << round_limit
            << " status=" << (result.ok() ? "PASS" : "FAIL")
            << " error=" << static_cast<int>(result.error)
            << " rounds=" << result.refinement_rounds
            << " source_m2=" << result.source_signed_area_m2
            << " planar_m2=" << result.planar_signed_area_m2
            << " abs_error_m2=" << result.absolute_area_error_m2
            << " allowed_m2=" << result.allowed_area_error_m2
            << " vertices=" << result.projected_vertices
            << " subdivision_error=" << static_cast<int>(result.subdivision_error)
            << " sample_error=" << static_cast<int>(result.sample_error)
            << '\n';

        if (result.ok()) break;
        if (result.error != aeris::projection::RingProjectionError::area_budget_unmet) break;
    }
}

}  // namespace

int main(const int argc, char** const argv) {
    if (argc < 2 || argc > 4) {
        std::cerr << "usage: aeris_shapefile_projection_probe <file.shp> [record-number] [ring-index]\n";
        return EXIT_FAILURE;
    }

    std::uint32_t record_number = 3U;
    std::size_t ring_index = 0U;
    if (argc >= 3 && !parse_u32(argv[2], record_number)) {
        std::cerr << "invalid record number\n";
        return EXIT_FAILURE;
    }
    if (argc >= 4 && !parse_size(argv[3], ring_index)) {
        std::cerr << "invalid ring index\n";
        return EXIT_FAILURE;
    }

    const auto source = aeris::source::read_polygon_shapefile(std::filesystem::path(argv[1]));
    if (!source.ok()) {
        std::cerr
            << "Shapefile parse failed: error=" << static_cast<int>(source.error)
            << " record=" << source.failed_record_number
            << " geographic_error=" << static_cast<int>(source.geographic_error)
            << " diagnostic=" << source.diagnostic
            << '\n';
        return EXIT_FAILURE;
    }

    const auto* const record = find_record(source, record_number);
    if (record == nullptr || ring_index >= record->rings.size()) {
        std::cerr << "requested record/ring does not exist\n";
        return EXIT_FAILURE;
    }

    const auto& source_ring = record->rings[ring_index];
    std::cout
        << "record=" << record_number
        << " ring=" << ring_index
        << " role=" << static_cast<int>(source_ring.role)
        << " winding=" << source_ring.geometry.longitude_winding
        << " input_vertices=" << source_ring.geometry.vertices.size()
        << " normalized_boundary_coordinates=" << source.normalized_boundary_coordinates
        << '\n';
    print_ring_geometry(source_ring);
    print_seam_crossings(source_ring.geometry, 0.0);

    auto projection_ring = source_ring.geometry;
    projection_ring.interior_side =
        source_ring.role == aeris::source::RingRole::exterior
            ? aeris::geometry::RingInteriorSide::right
            : aeris::geometry::RingInteriorSide::left;

    for (const auto* adapter : aeris::projection::builtin_projection_adapters()) {
        if (adapter != nullptr) probe_adapter(projection_ring, *adapter);
    }
    return EXIT_SUCCESS;
}
