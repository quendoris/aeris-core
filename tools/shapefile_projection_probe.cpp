// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/projection/ring.hpp"
#include "aeris/source/shapefile.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string_view>

namespace {

[[nodiscard]] bool parse_u32(const std::string_view text, std::uint32_t& value) noexcept {
    if (text.empty()) {
        return false;
    }
    const char* const first = text.data();
    const char* const last = text.data() + text.size();
    const auto parsed = std::from_chars(first, last, value);
    return parsed.ec == std::errc{} && parsed.ptr == last;
}

[[nodiscard]] bool parse_size(const std::string_view text, std::size_t& value) noexcept {
    if (text.empty()) {
        return false;
    }
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
        if (record.record_number == record_number) {
            return &record;
        }
    }
    return nullptr;
}

void probe_primitive(
    const aeris::geometry::LinearRing& ring,
    const aeris::projection::EqualAreaPrimitive primitive
) {
    aeris::projection::RingProjectionOptions options{};
    options.primitive = primitive;
    options.relative_area_tolerance = 1e-7;
    options.absolute_area_tolerance_m2 = 10'000.0;
    options.initial_geometric_tolerance_m = 500.0;
    options.initial_local_area_tolerance_m2 = 100'000'000.0;
    options.subdivision_max_depth = 30U;
    options.subdivision_max_segments_per_edge = 262'144U;

    std::cout
        << "primitive=" << static_cast<int>(primitive)
        << "\n";

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

        if (result.ok()) {
            break;
        }
        if (result.error != aeris::projection::RingProjectionError::area_budget_unmet) {
            break;
        }
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

    probe_primitive(source_ring.geometry, aeris::projection::EqualAreaPrimitive::sinusoidal);
    probe_primitive(source_ring.geometry, aeris::projection::EqualAreaPrimitive::mollweide);
    return EXIT_SUCCESS;
}
