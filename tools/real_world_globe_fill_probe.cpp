// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/geo/rotation.hpp"
#include "aeris/geo/wgs84.hpp"
#include "aeris/source/acquisition.hpp"
#include "aeris/source/natural_earth.hpp"
#include "aeris/source/registry.hpp"
#include "aeris/view/globe_curve.hpp"
#include "aeris/view/globe_polygon.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

constexpr std::string_view kProvider = "Natural Earth";
constexpr std::string_view kDataset = "ne_110m_land";
constexpr std::string_view kSnapshot = "v5.1.2";
constexpr std::string_view kAdapterId = "natural-earth.ne-110m-land.shapefile.v1";
constexpr std::string_view kSourceUri =
    "https://github.com/nvkelso/natural-earth-vector/tree/f1890d9f152c896d250a77557a5751a93d494776/110m_physical";

struct VisibleFeature final {
    std::vector<std::vector<aeris::geometry::PlanarPoint>> fill_rings;
    std::vector<std::vector<aeris::geometry::PlanarPoint>> coastline_parts;
};

struct Summary final {
    std::size_t features = 0U;
    std::size_t source_rings = 0U;
    std::size_t source_vertices = 0U;
    std::size_t visible_fill_source_rings = 0U;
    std::size_t partial_fill_source_rings = 0U;
    std::size_t fill_rings = 0U;
    std::size_t fill_vertices = 0U;
    std::size_t horizon_arc_segments = 0U;
    std::size_t horizon_crossings = 0U;
    std::size_t coastline_parts = 0U;
    std::size_t coastline_vertices = 0U;

    unsigned max_refinement_rounds = 0U;
    double min_final_curve_tolerance_m =
        std::numeric_limits<double>::infinity();
    double min_final_horizon_arc_tolerance_m =
        std::numeric_limits<double>::infinity();
    double max_estimated_area_delta_m2 = 0.0;
    double max_allowed_area_delta_m2 = 0.0;
};

[[nodiscard]] double radians(const double degrees) noexcept {
    return degrees * aeris::geo::kPi / 180.0;
}

[[nodiscard]] std::string trim_ascii(std::string value) {
    const std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const std::size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1U);
}

[[nodiscard]] bool parse_size(
    const std::string_view text,
    std::uintmax_t& value
) noexcept {
    if (text.empty()) {
        return false;
    }
    const char* const first = text.data();
    const char* const last = text.data() + text.size();
    const auto parsed = std::from_chars(first, last, value);
    return parsed.ec == std::errc{} && parsed.ptr == last;
}

[[nodiscard]] bool split_resource_line(
    const std::string& line,
    std::string& logical_name,
    std::string& relative_path,
    std::string& sha256,
    std::uintmax_t& size_bytes
) {
    const std::size_t first = line.find('\t');
    if (first == std::string::npos) {
        return false;
    }
    const std::size_t second = line.find('\t', first + 1U);
    if (second == std::string::npos) {
        return false;
    }
    const std::size_t third = line.find('\t', second + 1U);
    if (third == std::string::npos ||
        line.find('\t', third + 1U) != std::string::npos) {
        return false;
    }

    logical_name = line.substr(0U, first);
    relative_path = line.substr(first + 1U, second - first - 1U);
    sha256 = line.substr(second + 1U, third - second - 1U);
    const std::string_view size_text(
        line.data() + third + 1U,
        line.size() - third - 1U
    );
    return !logical_name.empty() && !relative_path.empty() &&
           !sha256.empty() && parse_size(size_text, size_bytes);
}

[[nodiscard]] bool load_pin_resources(
    const std::filesystem::path& pin_directory,
    aeris::source::SnapshotManifest& manifest
) {
    std::ifstream input(pin_directory / "resources.tsv", std::ios::binary);
    if (!input) {
        std::cerr << "unable to open pin resource manifest\n";
        return false;
    }

    std::string line;
    std::size_t line_number = 0U;
    while (std::getline(input, line)) {
        ++line_number;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty() || line.front() == '#') {
            continue;
        }

        std::string logical_name;
        std::string relative_path;
        std::string sha256;
        std::uintmax_t size_bytes = 0U;
        if (!split_resource_line(
                line,
                logical_name,
                relative_path,
                sha256,
                size_bytes
            )) {
            std::cerr << "invalid resources.tsv line " << line_number << '\n';
            return false;
        }

        aeris::source::ResourceSpec resource{};
        resource.logical_name = std::move(logical_name);
        resource.relative_path = std::filesystem::path(std::move(relative_path));
        resource.sha256 = std::move(sha256);
        resource.size_bytes = size_bytes;
        manifest.resources.push_back(std::move(resource));
    }

    return input.eof() && !manifest.resources.empty();
}

[[nodiscard]] std::string load_expected_content_hash(
    const std::filesystem::path& pin_directory
) {
    std::ifstream input(pin_directory / "content.sha256", std::ios::binary);
    if (!input) {
        return {};
    }
    std::string value;
    std::getline(input, value);
    if (!input && !input.eof()) {
        return {};
    }
    return trim_ascii(std::move(value));
}

[[nodiscard]] bool verify_fill_geometry(
    const aeris::view::GlobePolygonResult& polygon,
    const double radius_m,
    const std::string& feature_id,
    const std::size_t ring_index
) {
    const double radial_tolerance_m = 1.0;
    for (const auto& ring : polygon.rings) {
        if (ring.size() < 3U) {
            std::cerr
                << "degenerate fill ring: feature=" << feature_id
                << " ring=" << ring_index << '\n';
            return false;
        }
        for (const auto point : ring) {
            const double radial = std::hypot(point.x, point.y);
            if (!std::isfinite(radial) || radial > radius_m + radial_tolerance_m) {
                std::cerr
                    << std::setprecision(17)
                    << "fill point escaped globe disk: feature=" << feature_id
                    << " ring=" << ring_index
                    << " radial_m=" << radial
                    << " radius_m=" << radius_m << '\n';
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] bool build_visible_world(
    const aeris::source::Result& source,
    const aeris::geo::Mat3& world_to_view,
    const double radius_m,
    Summary& summary,
    std::vector<VisibleFeature>& visible_features
) {
    aeris::view::VerifiedGlobePolygonOptions verified_options{};
    verified_options.initial.curve.geometric_tolerance_m = 5'000.0;
    verified_options.initial.curve.horizon_tolerance_m = 0.01;
    verified_options.initial.curve.max_subdivision_depth = 32U;
    verified_options.initial.curve.max_root_iterations = 80U;
    verified_options.initial.curve.max_segments = 1'000'000U;
    verified_options.initial.horizon_arc_tolerance_m = 500.0;
    verified_options.initial.max_horizon_arc_segments = 1'000'000U;
    verified_options.initial.max_output_rings = 4096U;
    verified_options.relative_area_stability_tolerance = 5e-3;
    verified_options.absolute_area_stability_tolerance_m2 = 1.0;
    verified_options.max_refinement_rounds = 18U;

    visible_features.clear();
    visible_features.reserve(source.features.size());
    summary.features = source.features.size();

    for (const auto& feature : source.features) {
        VisibleFeature visible_feature{};

        for (std::size_t ring_index = 0U;
             ring_index < feature.rings.size();
             ++ring_index) {
            const auto& source_ring = feature.rings[ring_index];
            ++summary.source_rings;
            summary.source_vertices += source_ring.geometry.vertices.size();

            const auto verified =
                aeris::view::project_visible_wgs84_linear_polygon_ring_verified(
                    source_ring.geometry,
                    world_to_view,
                    verified_options,
                    radius_m
                );
            if (!verified.ok()) {
                const auto& polygon = verified.polygon;
                std::cerr
                    << std::setprecision(17)
                    << "verified globe fill failure: feature=" << feature.stable_id
                    << " ring=" << ring_index
                    << " verification_error=" << static_cast<int>(verified.error)
                    << " finite_error=" << static_cast<int>(polygon.error)
                    << " geographic_error=" << static_cast<int>(polygon.geographic_error)
                    << " curve_error=" << static_cast<int>(polygon.curve_error)
                    << " sample_error=" << static_cast<int>(polygon.sample_error)
                    << " source_m2=" << polygon.source_signed_area_m2
                    << " planar_m2=" << polygon.planar_signed_area_m2
                    << " disk_m2=" << polygon.visible_disk_area_m2
                    << " output_rings=" << polygon.rings.size()
                    << " crossings=" << polygon.horizon_crossings
                    << " arc_segments=" << polygon.horizon_arc_segments
                    << " vertices=" << polygon.projected_vertices
                    << " rounds=" << verified.refinement_rounds
                    << " final_curve_tol_m="
                    << verified.final_curve_geometric_tolerance_m
                    << " final_arc_tol_m="
                    << verified.final_horizon_arc_tolerance_m
                    << " estimated_area_delta_m2="
                    << verified.estimated_planar_area_error_m2
                    << " allowed_area_delta_m2="
                    << verified.allowed_planar_area_delta_m2
                    << '\n';
                return false;
            }

            const auto& polygon = verified.polygon;
            if (!verify_fill_geometry(
                    polygon,
                    radius_m,
                    feature.stable_id,
                    ring_index
                )) {
                return false;
            }

            aeris::view::GlobeCurveOptions coastline_options =
                verified_options.initial.curve;
            coastline_options.geometric_tolerance_m =
                verified.final_curve_geometric_tolerance_m;
            const auto coastline = aeris::view::project_visible_wgs84_linear_ring(
                source_ring.geometry,
                world_to_view,
                coastline_options,
                radius_m
            );
            if (!coastline.ok()) {
                std::cerr
                    << "coastline verification failed after fill success: feature="
                    << feature.stable_id << " ring=" << ring_index
                    << " error=" << static_cast<int>(coastline.error) << '\n';
                return false;
            }
            if (coastline.horizon_crossings != polygon.horizon_crossings) {
                std::cerr
                    << "fill/coast horizon crossing mismatch: feature="
                    << feature.stable_id << " ring=" << ring_index
                    << " fill=" << polygon.horizon_crossings
                    << " coast=" << coastline.horizon_crossings << '\n';
                return false;
            }

            summary.max_refinement_rounds = std::max(
                summary.max_refinement_rounds,
                verified.refinement_rounds
            );
            summary.min_final_curve_tolerance_m = std::min(
                summary.min_final_curve_tolerance_m,
                verified.final_curve_geometric_tolerance_m
            );
            summary.min_final_horizon_arc_tolerance_m = std::min(
                summary.min_final_horizon_arc_tolerance_m,
                verified.final_horizon_arc_tolerance_m
            );
            summary.max_estimated_area_delta_m2 = std::max(
                summary.max_estimated_area_delta_m2,
                verified.estimated_planar_area_error_m2
            );
            summary.max_allowed_area_delta_m2 = std::max(
                summary.max_allowed_area_delta_m2,
                verified.allowed_planar_area_delta_m2
            );

            if (!polygon.rings.empty()) {
                ++summary.visible_fill_source_rings;
            }
            if (polygon.horizon_crossings > 0U) {
                ++summary.partial_fill_source_rings;
            }
            summary.fill_rings += polygon.rings.size();
            summary.fill_vertices += polygon.projected_vertices;
            summary.horizon_arc_segments += polygon.horizon_arc_segments;
            summary.horizon_crossings += polygon.horizon_crossings;
            summary.coastline_parts += coastline.visible_parts.size();
            summary.coastline_vertices += coastline.projected_vertices;

            for (const auto& ring : polygon.rings) {
                visible_feature.fill_rings.push_back(ring);
            }
            for (const auto& part : coastline.visible_parts) {
                visible_feature.coastline_parts.push_back(part);
            }
        }

        visible_features.push_back(std::move(visible_feature));
    }

    if (summary.features != 127U ||
        summary.source_rings != 128U ||
        summary.source_vertices != 5015U) {
        std::cerr
            << "unexpected pinned source cardinality: features="
            << summary.features << " rings=" << summary.source_rings
            << " vertices=" << summary.source_vertices << '\n';
        return false;
    }
    if (summary.visible_fill_source_rings == 0U ||
        summary.partial_fill_source_rings == 0U ||
        summary.fill_rings == 0U ||
        summary.horizon_arc_segments == 0U ||
        summary.horizon_crossings == 0U ||
        summary.max_refinement_rounds < 2U ||
        !std::isfinite(summary.min_final_curve_tolerance_m) ||
        !std::isfinite(summary.min_final_horizon_arc_tolerance_m)) {
        std::cerr
            << "real-world fill proof did not exercise verified horizon topology: visible="
            << summary.visible_fill_source_rings
            << " partial=" << summary.partial_fill_source_rings
            << " fill_rings=" << summary.fill_rings
            << " arc_segments=" << summary.horizon_arc_segments
            << " crossings=" << summary.horizon_crossings
            << " max_rounds=" << summary.max_refinement_rounds << '\n';
        return false;
    }
    return true;
}

[[nodiscard]] bool write_svg(
    const std::filesystem::path& output_path,
    const std::string& content_sha256,
    const std::vector<VisibleFeature>& features,
    const Summary& summary,
    const double radius_m,
    const double center_longitude_deg,
    const double center_latitude_deg
) {
    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    if (!output) {
        std::cerr << "unable to open filled globe SVG output\n";
        return false;
    }

    constexpr double center_x = 450.0;
    constexpr double center_y = 430.0;
    constexpr double screen_radius = 360.0;
    const double pixels_per_metre = screen_radius / radius_m;
    const auto screen_x = [&](const double x) {
        return center_x + x * pixels_per_metre;
    };
    const auto screen_y = [&](const double y) {
        return center_y - y * pixels_per_metre;
    };

    output
        << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"900\" height=\"920\" "
           "viewBox=\"0 0 900 920\" role=\"img\" "
           "aria-label=\"AERIS Natural Earth verified filled authalic orthographic globe proof\">\n"
        << "<style>"
           ".background{fill:#f5f5f3;}"
           ".ocean{fill:#ecebe7;stroke:none;}"
           ".land{fill:#d9d6ce;stroke:none;}"
           ".coast{fill:none;stroke:#222;stroke-width:.7;stroke-linecap:round;stroke-linejoin:round;}"
           ".limb{fill:none;stroke:#111;stroke-width:2;}"
           ".title{font:24px sans-serif;fill:#111;}"
           ".meta{font:14px monospace;fill:#444;}"
           "</style>\n"
        << "<rect class=\"background\" width=\"900\" height=\"920\"/>\n"
        << "<circle class=\"ocean\" cx=\"" << center_x
        << "\" cy=\"" << center_y
        << "\" r=\"" << screen_radius << "\"/>\n";

    output << std::fixed << std::setprecision(3);
    for (const auto& feature : features) {
        if (!feature.fill_rings.empty()) {
            output << "<path class=\"land\" fill-rule=\"evenodd\" d=\"";
            for (const auto& ring : feature.fill_rings) {
                if (ring.size() < 3U) {
                    continue;
                }
                output << 'M' << screen_x(ring.front().x) << ','
                       << screen_y(ring.front().y) << ' ';
                for (std::size_t index = 1U; index < ring.size(); ++index) {
                    output << 'L' << screen_x(ring[index].x) << ','
                           << screen_y(ring[index].y) << ' ';
                }
                output << 'Z';
            }
            output << "\"/>\n";
        }

        for (const auto& part : feature.coastline_parts) {
            if (part.size() < 2U) {
                continue;
            }
            output << "<path class=\"coast\" d=\"M"
                   << screen_x(part.front().x) << ','
                   << screen_y(part.front().y) << ' ';
            for (std::size_t index = 1U; index < part.size(); ++index) {
                output << 'L' << screen_x(part[index].x) << ','
                       << screen_y(part[index].y) << ' ';
            }
            output << "\"/>\n";
        }
    }

    output
        << "<circle class=\"limb\" cx=\"" << center_x
        << "\" cy=\"" << center_y
        << "\" r=\"" << screen_radius << "\"/>\n"
        << "<text class=\"title\" x=\"450\" y=\"42\" text-anchor=\"middle\">"
           "Natural Earth / Verified Visible-Land Globe</text>\n"
        << "<text class=\"meta\" x=\"40\" y=\"800\">snapshot SHA-256: "
        << content_sha256 << "</text>\n"
        << "<text class=\"meta\" x=\"40\" y=\"825\">camera center: "
        << center_longitude_deg << "° lon, " << center_latitude_deg
        << "° geodetic lat</text>\n"
        << "<text class=\"meta\" x=\"40\" y=\"850\">fill rings: "
        << summary.fill_rings << " / partial source rings: "
        << summary.partial_fill_source_rings << " / horizon crossings: "
        << summary.horizon_crossings << "</text>\n"
        << "<text class=\"meta\" x=\"40\" y=\"875\">fill vertices: "
        << summary.fill_vertices << " / horizon arc segments: "
        << summary.horizon_arc_segments << " / max refinement rounds: "
        << summary.max_refinement_rounds << "</text>\n"
        << "<text class=\"meta\" x=\"40\" y=\"900\">finest curve/arc tolerance: "
        << summary.min_final_curve_tolerance_m << " m / "
        << summary.min_final_horizon_arc_tolerance_m << " m</text>\n"
        << "</svg>\n";

    output.flush();
    return static_cast<bool>(output);
}

}  // namespace

int main(const int argc, char** const argv) {
    if (argc != 5) {
        std::cerr
            << "usage: aeris_real_world_globe_fill_probe "
               "<snapshot-root> <pin-directory> <output.svg> <retrieved-at-utc>\n";
        return EXIT_FAILURE;
    }

    const std::filesystem::path snapshot_root(argv[1]);
    const std::filesystem::path pin_directory(argv[2]);
    const std::filesystem::path output_path(argv[3]);
    const std::string retrieved_at_utc(argv[4]);

    aeris::source::SnapshotManifest manifest{};
    manifest.provider = std::string(kProvider);
    manifest.dataset = std::string(kDataset);
    manifest.snapshot = std::string(kSnapshot);
    manifest.source_uri = std::string(kSourceUri);
    manifest.retrieved_at_utc = retrieved_at_utc;
    if (!load_pin_resources(pin_directory, manifest)) {
        return EXIT_FAILURE;
    }

    const std::string expected_content_sha256 =
        load_expected_content_hash(pin_directory);
    if (expected_content_sha256.empty()) {
        std::cerr << "content.sha256 is missing or empty\n";
        return EXIT_FAILURE;
    }

    auto verified = aeris::source::verify_local_snapshot(snapshot_root, manifest);
    if (!verified.ok()) {
        std::cerr
            << "snapshot verification failed: error="
            << static_cast<int>(verified.error)
            << " resource=" << verified.failed_resource
            << " diagnostic=" << verified.diagnostic << '\n';
        return EXIT_FAILURE;
    }
    if (verified.snapshot->content_sha256() != expected_content_sha256) {
        std::cerr << "aggregate content identity mismatch\n";
        return EXIT_FAILURE;
    }

    aeris::source::AdapterRegistry registry{};
    if (registry.add(std::make_unique<aeris::source::NaturalEarthLand110mAdapter>()) !=
        aeris::source::RegistryError::none) {
        std::cerr << "unable to register Natural Earth adapter\n";
        return EXIT_FAILURE;
    }

    aeris::source::SourceBinding binding{};
    binding.adapter_id = std::string(kAdapterId);
    binding.capability = aeris::source::Capability::land;
    binding.snapshot = std::string(kSnapshot);
    binding.expected_content_sha256 = expected_content_sha256;
    const auto loaded = registry.load(binding, *verified.snapshot);
    if (!loaded.ok()) {
        std::cerr
            << "registry load failed: error=" << static_cast<int>(loaded.error)
            << " source_error=" << static_cast<int>(loaded.source_error)
            << " diagnostic=" << loaded.diagnostic << '\n';
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

    const double radius_m = aeris::geo::authalic_radius_m();
    Summary summary{};
    std::vector<VisibleFeature> visible_features;
    if (!build_visible_world(
            loaded.source,
            world_to_view,
            radius_m,
            summary,
            visible_features
        )) {
        return EXIT_FAILURE;
    }

    if (!write_svg(
            output_path,
            expected_content_sha256,
            visible_features,
            summary,
            radius_m,
            center_longitude_deg,
            center_latitude_deg
        )) {
        return EXIT_FAILURE;
    }

    std::cout
        << std::setprecision(17)
        << "real_world_globe_fill_probe: PASS\n"
        << "features=" << summary.features << '\n'
        << "source_rings=" << summary.source_rings << '\n'
        << "source_vertices=" << summary.source_vertices << '\n'
        << "visible_fill_source_rings="
        << summary.visible_fill_source_rings << '\n'
        << "partial_fill_source_rings="
        << summary.partial_fill_source_rings << '\n'
        << "fill_rings=" << summary.fill_rings << '\n'
        << "fill_vertices=" << summary.fill_vertices << '\n'
        << "horizon_arc_segments=" << summary.horizon_arc_segments << '\n'
        << "horizon_crossings=" << summary.horizon_crossings << '\n'
        << "coastline_parts=" << summary.coastline_parts << '\n'
        << "coastline_vertices=" << summary.coastline_vertices << '\n'
        << "max_refinement_rounds=" << summary.max_refinement_rounds << '\n'
        << "min_final_curve_tolerance_m="
        << summary.min_final_curve_tolerance_m << '\n'
        << "min_final_horizon_arc_tolerance_m="
        << summary.min_final_horizon_arc_tolerance_m << '\n'
        << "max_estimated_area_delta_m2="
        << summary.max_estimated_area_delta_m2 << '\n'
        << "max_allowed_area_delta_m2="
        << summary.max_allowed_area_delta_m2 << '\n'
        << "camera_center_longitude_deg=" << center_longitude_deg << '\n'
        << "camera_center_geodetic_latitude_deg=" << center_latitude_deg << '\n'
        << "svg=" << output_path.string() << '\n';

    return EXIT_SUCCESS;
}
