// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/geo/wgs84.hpp"
#include "aeris/projection/ring.hpp"
#include "aeris/source/natural_earth.hpp"
#include "aeris/source/registry.hpp"

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

struct ProjectedFeature final {
    std::vector<std::vector<aeris::geometry::PlanarPoint>> rings;
};

struct ProjectionSummary final {
    double semantic_source_area_m2 = 0.0;
    double semantic_planar_area_m2 = 0.0;
    double aggregate_abs_error_m2 = 0.0;
    double allowed_error_sum_m2 = 0.0;
    double max_ring_abs_error_m2 = 0.0;
    std::size_t rings = 0U;
    std::size_t input_vertices = 0U;
    std::size_t projected_vertices = 0U;
    std::size_t projected_pieces = 0U;
    std::size_t split_rings = 0U;
    std::size_t seam_crossings = 0U;
    std::size_t winding_rings = 0U;
};

struct Panel final {
    double center_x = 0.0;
    double center_y = 0.0;
};

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
    if (third == std::string::npos || line.find('\t', third + 1U) != std::string::npos) {
        return false;
    }

    logical_name = line.substr(0U, first);
    relative_path = line.substr(first + 1U, second - first - 1U);
    sha256 = line.substr(second + 1U, third - second - 1U);
    const std::string_view size_text(line.data() + third + 1U, line.size() - third - 1U);
    return !logical_name.empty() && !relative_path.empty() && !sha256.empty() &&
           parse_size(size_text, size_bytes);
}

[[nodiscard]] bool load_pin_resources(
    const std::filesystem::path& pin_directory,
    aeris::source::SnapshotManifest& manifest
) {
    const std::filesystem::path path = pin_directory / "resources.tsv";
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::cerr << "unable to open pin resource manifest: " << path << '\n';
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

    if (!input.eof()) {
        std::cerr << "I/O error while reading pin resource manifest\n";
        return false;
    }
    if (manifest.resources.empty()) {
        std::cerr << "pin resource manifest contains no resources\n";
        return false;
    }
    return true;
}

[[nodiscard]] std::string load_expected_content_hash(
    const std::filesystem::path& pin_directory
) {
    const std::filesystem::path path = pin_directory / "content.sha256";
    std::ifstream input(path, std::ios::binary);
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

[[nodiscard]] bool project_world(
    const aeris::source::Result& source,
    const aeris::projection::EqualAreaPrimitive primitive,
    const double central_meridian_rad,
    ProjectionSummary& summary,
    std::vector<ProjectedFeature>& projected_features
) {
    aeris::projection::RingProjectionOptions options{};
    options.primitive = primitive;
    options.central_meridian_rad = central_meridian_rad;
    options.relative_area_tolerance = 1e-7;
    options.absolute_area_tolerance_m2 = 10'000.0;
    options.initial_geometric_tolerance_m = 500.0;
    options.initial_local_area_tolerance_m2 = 100'000'000.0;
    options.max_refinement_rounds = 18U;
    options.subdivision_max_depth = 30U;
    options.subdivision_max_segments_per_edge = 262'144U;
    options.max_projection_pieces = 4096U;

    projected_features.clear();
    projected_features.reserve(source.features.size());

    for (const aeris::source::Feature& feature : source.features) {
        ProjectedFeature projected_feature{};
        projected_feature.rings.reserve(feature.rings.size());

        for (std::size_t ring_index = 0U; ring_index < feature.rings.size(); ++ring_index) {
            const aeris::source::FeatureRing& source_ring = feature.rings[ring_index];
            if (source_ring.geometry.longitude_winding != 0) {
                ++summary.winding_rings;
            }

            aeris::projection::PiecewiseRingProjectionResult projected =
                aeris::projection::project_wgs84_linear_ring_piecewise_verified(
                    source_ring.geometry,
                    options
                );
            if (!projected.ok()) {
                std::cerr
                    << std::setprecision(17)
                    << "projection failure: primitive=" << static_cast<int>(primitive)
                    << " central_meridian_rad=" << central_meridian_rad
                    << " feature=" << feature.stable_id
                    << " ring=" << ring_index
                    << " winding=" << source_ring.geometry.longitude_winding
                    << " error=" << static_cast<int>(projected.error)
                    << " seam_error=" << static_cast<int>(projected.seam_error)
                    << " piece_error=" << static_cast<int>(projected.piece_error)
                    << " geographic_error=" << static_cast<int>(projected.geographic_error)
                    << " subdivision_error=" << static_cast<int>(projected.subdivision_error)
                    << " sample_error=" << static_cast<int>(projected.sample_error)
                    << " failed_piece=" << projected.failed_piece
                    << " failed_edge=" << projected.failed_edge
                    << " source_m2=" << projected.source_signed_area_m2
                    << " planar_m2=" << projected.planar_signed_area_m2
                    << " seam_partition_error_m2=" << projected.seam_partition_error_m2
                    << " abs_error_m2=" << projected.absolute_area_error_m2
                    << " allowed_m2=" << projected.allowed_area_error_m2
                    << " pieces=" << projected.projected_pieces
                    << " crossings=" << projected.seam_crossings
                    << " rounds=" << projected.max_piece_refinement_rounds
                    << " vertices=" << projected.projected_vertices
                    << '\n';
                return false;
            }

            if ((projected.source_signed_area_m2 < 0.0) !=
                (projected.planar_signed_area_m2 < 0.0)) {
                std::cerr
                    << "orientation changed under equal-area primitive: feature="
                    << feature.stable_id << " ring=" << ring_index << '\n';
                return false;
            }

            const double semantic_sign =
                source_ring.role == aeris::source::RingRole::exterior ? 1.0 : -1.0;
            summary.semantic_source_area_m2 +=
                semantic_sign * std::abs(projected.source_signed_area_m2);
            summary.semantic_planar_area_m2 +=
                semantic_sign * std::abs(projected.planar_signed_area_m2);
            summary.allowed_error_sum_m2 += projected.allowed_area_error_m2;
            summary.max_ring_abs_error_m2 = std::max(
                summary.max_ring_abs_error_m2,
                projected.absolute_area_error_m2
            );
            ++summary.rings;
            summary.input_vertices += source_ring.geometry.vertices.size();
            summary.projected_vertices += projected.projected_vertices;
            summary.projected_pieces += projected.projected_pieces;
            summary.seam_crossings += projected.seam_crossings;
            if (projected.projected_pieces > 1U) {
                ++summary.split_rings;
            }

            for (auto& piece : projected.pieces) {
                projected_feature.rings.push_back(std::move(piece));
            }
        }

        projected_features.push_back(std::move(projected_feature));
    }

    summary.aggregate_abs_error_m2 = std::abs(
        summary.semantic_planar_area_m2 - summary.semantic_source_area_m2
    );
    if (!std::isfinite(summary.aggregate_abs_error_m2) ||
        summary.aggregate_abs_error_m2 > summary.allowed_error_sum_m2) {
        std::cerr
            << "aggregate semantic area error exceeds sum of per-ring budgets: error="
            << summary.aggregate_abs_error_m2
            << " allowed=" << summary.allowed_error_sum_m2 << '\n';
        return false;
    }

    return true;
}

void write_projected_features(
    std::ofstream& output,
    const std::vector<ProjectedFeature>& features,
    const Panel panel,
    const double pixels_per_metre,
    const std::string_view clip_id
) {
    output << "<g clip-path=\"url(#" << clip_id << ")\">\n";
    output << std::fixed << std::setprecision(3);

    for (const ProjectedFeature& feature : features) {
        bool has_points = false;
        for (const auto& ring : feature.rings) {
            if (!ring.empty()) {
                has_points = true;
                break;
            }
        }
        if (!has_points) {
            continue;
        }

        output << "<path class=\"land\" fill-rule=\"evenodd\" d=\"";
        for (const auto& ring : feature.rings) {
            if (ring.empty()) {
                continue;
            }
            const auto screen_x = [&](const double x) {
                return panel.center_x + x * pixels_per_metre;
            };
            const auto screen_y = [&](const double y) {
                return panel.center_y - y * pixels_per_metre;
            };

            output << 'M' << screen_x(ring.front().x) << ',' << screen_y(ring.front().y) << ' ';
            for (std::size_t index = 1U; index < ring.size(); ++index) {
                output << 'L' << screen_x(ring[index].x) << ',' << screen_y(ring[index].y) << ' ';
            }
            output << 'Z';
        }
        output << "\"/>\n";
    }

    output << "</g>\n";
}

void write_sinusoidal_outline(
    std::ofstream& output,
    const Panel panel,
    const double pixels_per_metre,
    const double radius_m
) {
    constexpr int samples = 180;
    output << std::fixed << std::setprecision(3);
    output << "<path id=\"sinusoidal-outline\" d=\"";

    for (int index = 0; index <= samples; ++index) {
        const double t = static_cast<double>(index) / static_cast<double>(samples);
        const double beta = -aeris::geo::kHalfPi + t * aeris::geo::kPi;
        const double x = aeris::geo::kPi * radius_m * std::cos(beta);
        const double y = radius_m * beta;
        const double sx = panel.center_x + x * pixels_per_metre;
        const double sy = panel.center_y - y * pixels_per_metre;
        output << (index == 0 ? 'M' : 'L') << sx << ',' << sy << ' ';
    }
    for (int index = samples; index >= 0; --index) {
        const double t = static_cast<double>(index) / static_cast<double>(samples);
        const double beta = -aeris::geo::kHalfPi + t * aeris::geo::kPi;
        const double x = -aeris::geo::kPi * radius_m * std::cos(beta);
        const double y = radius_m * beta;
        const double sx = panel.center_x + x * pixels_per_metre;
        const double sy = panel.center_y - y * pixels_per_metre;
        output << 'L' << sx << ',' << sy << ' ';
    }
    output << "Z\"/>\n";
}

[[nodiscard]] bool write_svg(
    const std::filesystem::path& output_path,
    const aeris::source::Result& source,
    const std::string& content_sha256,
    const ProjectionSummary& sinusoidal_summary,
    const ProjectionSummary& mollweide_summary,
    const std::vector<ProjectedFeature>& sinusoidal,
    const std::vector<ProjectedFeature>& mollweide
) {
    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    if (!output) {
        std::cerr << "unable to open SVG output: " << output_path << '\n';
        return false;
    }

    const double radius_m = aeris::geo::authalic_radius_m();
    const double pixels_per_metre = 370.0 / (aeris::geo::kPi * radius_m);
    const Panel sinusoidal_panel{450.0, 340.0};
    const Panel mollweide_panel{1350.0, 340.0};
    const double mollweide_rx =
        2.0 * std::sqrt(2.0) * radius_m * pixels_per_metre;
    const double mollweide_ry =
        std::sqrt(2.0) * radius_m * pixels_per_metre;

    output
        << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"1800\" height=\"700\" "
           "viewBox=\"0 0 1800 700\" role=\"img\" "
           "aria-label=\"AERIS real Natural Earth equal-area integration proof\">\n"
        << "<style>\n"
           ".background{fill:#f5f5f3;}"
           ".land{fill:#d9d6ce;stroke:#252525;stroke-width:.45;vector-effect:non-scaling-stroke;}"
           ".outline{fill:none;stroke:#111;stroke-width:1.5;vector-effect:non-scaling-stroke;}"
           ".title{font:24px sans-serif;fill:#111;}"
           ".meta{font:14px monospace;fill:#444;}"
           "</style>\n"
        << "<rect class=\"background\" width=\"1800\" height=\"700\"/>\n"
        << "<defs>\n";

    write_sinusoidal_outline(output, sinusoidal_panel, pixels_per_metre, radius_m);
    output
        << "<clipPath id=\"clip-sinusoidal\"><use href=\"#sinusoidal-outline\"/></clipPath>\n"
        << "<clipPath id=\"clip-mollweide\"><ellipse cx=\"" << mollweide_panel.center_x
        << "\" cy=\"" << mollweide_panel.center_y
        << "\" rx=\"" << mollweide_rx
        << "\" ry=\"" << mollweide_ry << "\"/></clipPath>\n"
        << "</defs>\n";

    output
        << "<text class=\"title\" x=\"" << sinusoidal_panel.center_x
        << "\" y=\"54\" text-anchor=\"middle\">Natural Earth / Sinusoidal</text>\n"
        << "<text class=\"title\" x=\"" << mollweide_panel.center_x
        << "\" y=\"54\" text-anchor=\"middle\">Natural Earth / Mollweide</text>\n";

    write_projected_features(
        output,
        sinusoidal,
        sinusoidal_panel,
        pixels_per_metre,
        "clip-sinusoidal"
    );
    write_projected_features(
        output,
        mollweide,
        mollweide_panel,
        pixels_per_metre,
        "clip-mollweide"
    );

    output
        << "<use href=\"#sinusoidal-outline\" class=\"outline\"/>\n"
        << "<ellipse class=\"outline\" cx=\"" << mollweide_panel.center_x
        << "\" cy=\"" << mollweide_panel.center_y
        << "\" rx=\"" << mollweide_rx
        << "\" ry=\"" << mollweide_ry << "\"/>\n";

    output << std::scientific << std::setprecision(6);
    output
        << "<text class=\"meta\" x=\"60\" y=\"605\">snapshot: "
        << source.provenance.snapshot << " / dataset version "
        << source.provenance.dataset_version << "</text>\n"
        << "<text class=\"meta\" x=\"60\" y=\"630\">aggregate SHA-256: "
        << content_sha256 << "</text>\n"
        << "<text class=\"meta\" x=\"60\" y=\"655\">sin area error: "
        << sinusoidal_summary.aggregate_abs_error_m2
        << " m² / moll area error: "
        << mollweide_summary.aggregate_abs_error_m2 << " m²</text>\n"
        << "<text class=\"meta\" x=\"60\" y=\"680\">features: "
        << source.features.size() << " / rings: "
        << sinusoidal_summary.rings << " / pieces: "
        << sinusoidal_summary.projected_pieces << " / seam crossings: "
        << sinusoidal_summary.seam_crossings << "</text>\n"
        << "</svg>\n";

    output.flush();
    return static_cast<bool>(output);
}

}  // namespace

int main(const int argc, char** const argv) {
    if (argc != 5) {
        std::cerr
            << "usage: aeris_real_world_probe "
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

    aeris::source::SnapshotVerificationResult verified =
        aeris::source::verify_local_snapshot(snapshot_root, manifest);
    if (!verified.ok()) {
        std::cerr
            << "snapshot verification failed: error="
            << static_cast<int>(verified.error)
            << " resource=" << verified.failed_resource
            << " diagnostic=" << verified.diagnostic << '\n';
        return EXIT_FAILURE;
    }

    const aeris::source::VerifiedSnapshot& snapshot = *verified.snapshot;
    if (snapshot.content_sha256() != expected_content_sha256) {
        std::cerr
            << "aggregate content identity mismatch: expected="
            << expected_content_sha256
            << " actual=" << snapshot.content_sha256() << '\n';
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

    const aeris::source::RegistryLoadResult loaded =
        registry.load(binding, snapshot);
    if (!loaded.ok()) {
        std::cerr
            << "registry load failed: error=" << static_cast<int>(loaded.error)
            << " source_error=" << static_cast<int>(loaded.source_error)
            << " diagnostic=" << loaded.diagnostic << '\n';
        return EXIT_FAILURE;
    }
    if (loaded.source.features.empty()) {
        std::cerr << "Natural Earth adapter emitted no features\n";
        return EXIT_FAILURE;
    }

    ProjectionSummary sinusoidal_summary{};
    ProjectionSummary mollweide_summary{};
    std::vector<ProjectedFeature> sinusoidal;
    std::vector<ProjectedFeature> mollweide;

    if (!project_world(
            loaded.source,
            aeris::projection::EqualAreaPrimitive::sinusoidal,
            0.0,
            sinusoidal_summary,
            sinusoidal
        ) ||
        !project_world(
            loaded.source,
            aeris::projection::EqualAreaPrimitive::mollweide,
            0.0,
            mollweide_summary,
            mollweide
        )) {
        return EXIT_FAILURE;
    }

    ProjectionSummary stress_sinusoidal_summary{};
    ProjectionSummary stress_mollweide_summary{};
    std::vector<ProjectedFeature> stress_sinusoidal;
    std::vector<ProjectedFeature> stress_mollweide;
    constexpr double stress_central_meridian_rad = aeris::geo::kHalfPi;

    if (!project_world(
            loaded.source,
            aeris::projection::EqualAreaPrimitive::sinusoidal,
            stress_central_meridian_rad,
            stress_sinusoidal_summary,
            stress_sinusoidal
        ) ||
        !project_world(
            loaded.source,
            aeris::projection::EqualAreaPrimitive::mollweide,
            stress_central_meridian_rad,
            stress_mollweide_summary,
            stress_mollweide
        )) {
        return EXIT_FAILURE;
    }

    if (stress_sinusoidal_summary.split_rings == 0U ||
        stress_sinusoidal_summary.seam_crossings == 0U ||
        stress_mollweide_summary.split_rings == 0U ||
        stress_mollweide_summary.seam_crossings == 0U) {
        std::cerr
            << "shifted-seam real-world stress did not exercise generic splitting: "
            << "sin_split_rings=" << stress_sinusoidal_summary.split_rings
            << " sin_crossings=" << stress_sinusoidal_summary.seam_crossings
            << " moll_split_rings=" << stress_mollweide_summary.split_rings
            << " moll_crossings=" << stress_mollweide_summary.seam_crossings
            << '\n';
        return EXIT_FAILURE;
    }

    if (!write_svg(
            output_path,
            loaded.source,
            expected_content_sha256,
            sinusoidal_summary,
            mollweide_summary,
            sinusoidal,
            mollweide
        )) {
        std::cerr << "unable to write real-world diagnostic SVG\n";
        return EXIT_FAILURE;
    }

    std::cout << std::scientific << std::setprecision(12)
              << "real_world_probe: PASS\n"
              << "snapshot_content_sha256=" << snapshot.content_sha256() << '\n'
              << "features=" << loaded.source.features.size() << '\n'
              << "rings=" << sinusoidal_summary.rings << '\n'
              << "source_vertices=" << sinusoidal_summary.input_vertices << '\n'
              << "winding_rings=" << sinusoidal_summary.winding_rings << '\n'
              << "sinusoidal_projected_pieces=" << sinusoidal_summary.projected_pieces << '\n'
              << "mollweide_projected_pieces=" << mollweide_summary.projected_pieces << '\n'
              << "sinusoidal_split_rings=" << sinusoidal_summary.split_rings << '\n'
              << "mollweide_split_rings=" << mollweide_summary.split_rings << '\n'
              << "sinusoidal_seam_crossings=" << sinusoidal_summary.seam_crossings << '\n'
              << "mollweide_seam_crossings=" << mollweide_summary.seam_crossings << '\n'
              << "sinusoidal_projected_vertices=" << sinusoidal_summary.projected_vertices << '\n'
              << "mollweide_projected_vertices=" << mollweide_summary.projected_vertices << '\n'
              << "source_semantic_land_area_m2=" << sinusoidal_summary.semantic_source_area_m2 << '\n'
              << "sinusoidal_land_area_m2=" << sinusoidal_summary.semantic_planar_area_m2 << '\n'
              << "mollweide_land_area_m2=" << mollweide_summary.semantic_planar_area_m2 << '\n'
              << "sinusoidal_abs_error_m2=" << sinusoidal_summary.aggregate_abs_error_m2 << '\n'
              << "mollweide_abs_error_m2=" << mollweide_summary.aggregate_abs_error_m2 << '\n'
              << "sinusoidal_max_ring_error_m2=" << sinusoidal_summary.max_ring_abs_error_m2 << '\n'
              << "mollweide_max_ring_error_m2=" << mollweide_summary.max_ring_abs_error_m2 << '\n'
              << "stress_central_meridian_deg=9.000000000000e+01\n"
              << "stress_sinusoidal_projected_pieces=" << stress_sinusoidal_summary.projected_pieces << '\n'
              << "stress_mollweide_projected_pieces=" << stress_mollweide_summary.projected_pieces << '\n'
              << "stress_sinusoidal_split_rings=" << stress_sinusoidal_summary.split_rings << '\n'
              << "stress_mollweide_split_rings=" << stress_mollweide_summary.split_rings << '\n'
              << "stress_sinusoidal_seam_crossings=" << stress_sinusoidal_summary.seam_crossings << '\n'
              << "stress_mollweide_seam_crossings=" << stress_mollweide_summary.seam_crossings << '\n'
              << "stress_sinusoidal_abs_error_m2=" << stress_sinusoidal_summary.aggregate_abs_error_m2 << '\n'
              << "stress_mollweide_abs_error_m2=" << stress_mollweide_summary.aggregate_abs_error_m2 << '\n'
              << "svg=" << output_path.string() << '\n';

    return EXIT_SUCCESS;
}
