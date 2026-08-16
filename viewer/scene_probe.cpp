// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "scene_builder.hpp"
#include "unfold.hpp"
#include "world_loader.hpp"

#include "aeris/geo/rotation.hpp"
#include "aeris/geo/wgs84.hpp"
#include "aeris/geometry/planar.hpp"
#include "aeris/view/globe_polygon.hpp"

#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

[[nodiscard]] double radians(const double degrees) noexcept {
    return degrees * aeris::geo::kPi / 180.0;
}

[[nodiscard]] bool scene_has_expected_geometry(
    const aeris::viewer::SceneData& scene
) {
    if (!scene.ok || scene.canceled || scene.features.size() != 127U ||
        scene.fill_rings == 0U || scene.outline_parts == 0U ||
        scene.vertices == 0U) {
        return false;
    }

    if (scene.mode == aeris::viewer::ViewMode::globe) {
        return scene.globe_radius_m > 0.0 &&
               scene.max_refinement_rounds >= 2U;
    }

    return scene.max_x > scene.min_x && scene.max_y > scene.min_y;
}

[[nodiscard]] bool near(const double a, const double b, const double tolerance) {
    return std::isfinite(a) && std::isfinite(b) && std::abs(a - b) <= tolerance;
}

[[nodiscard]] bool unfold_has_expected_contract(
    const aeris::viewer::UnfoldBundle& bundle
) {
    if (!bundle.ok || bundle.canceled ||
        bundle.target_mode != aeris::viewer::ViewMode::mollweide ||
        !scene_has_expected_geometry(bundle.globe_endpoint) ||
        !scene_has_expected_geometry(bundle.flat_endpoint) ||
        bundle.guides.size() != 24U) {
        return false;
    }

    const aeris::viewer::UnfoldGuideLine* seam_left = nullptr;
    const aeris::viewer::UnfoldGuideLine* seam_right = nullptr;
    std::size_t seam_count = 0U;
    for (const auto& line : bundle.guides) {
        if (line.vertices.size() < 2U) {
            return false;
        }
        if (line.kind == aeris::viewer::UnfoldGuideKind::seam) {
            if (seam_count == 0U) {
                seam_left = &line;
            } else if (seam_count == 1U) {
                seam_right = &line;
            }
            ++seam_count;
        }
    }
    if (seam_count != 2U || seam_left == nullptr || seam_right == nullptr ||
        seam_left->vertices.size() != seam_right->vertices.size()) {
        return false;
    }

    constexpr double globe_tolerance_m = 1e-6;
    for (std::size_t index = 0U; index < seam_left->vertices.size(); ++index) {
        const auto& left = seam_left->vertices[index];
        const auto& right = seam_right->vertices[index];
        if (!near(left.globe.x, right.globe.x, globe_tolerance_m) ||
            !near(left.globe.y, right.globe.y, globe_tolerance_m)) {
            return false;
        }
    }

    const std::size_t mid = seam_left->vertices.size() / 2U;
    if (!(seam_left->vertices[mid].flat.x < 0.0 &&
          seam_right->vertices[mid].flat.x > 0.0)) {
        return false;
    }

    const auto& sample = bundle.guides.front().vertices[bundle.guides.front().vertices.size() / 3U];
    const auto at_start = aeris::viewer::interpolate_unfold_vertex(sample, 0.0);
    const auto at_end = aeris::viewer::interpolate_unfold_vertex(sample, 1.0);
    if (!near(at_start.x, sample.globe.x, 0.0) ||
        !near(at_start.y, sample.globe.y, 0.0) ||
        !near(at_end.x, sample.flat.x, 0.0) ||
        !near(at_end.y, sample.flat.y, 0.0)) {
        return false;
    }

    if (aeris::viewer::unfold_eased_progress(-1.0) != 0.0 ||
        aeris::viewer::unfold_eased_progress(0.0) != 0.0 ||
        aeris::viewer::unfold_eased_progress(1.0) != 1.0 ||
        aeris::viewer::unfold_eased_progress(2.0) != 1.0) {
        return false;
    }

    return true;
}

void print_scene_failure(
    const char* label,
    const aeris::viewer::SceneData& scene
) {
    std::cerr
        << "scene probe failed for " << label
        << ": ok=" << scene.ok
        << " diagnostic=" << scene.diagnostic
        << " camera=" << scene.camera_longitude_deg << ','
        << scene.camera_latitude_deg
        << " features=" << scene.features.size()
        << " fill_rings=" << scene.fill_rings
        << " outlines=" << scene.outline_parts
        << " vertices=" << scene.vertices
        << " max_refinement=" << scene.max_refinement_rounds
        << '\n';
}

void diagnose_camera_record_111(const aeris::source::Result& world) {
    const auto beta = aeris::geo::authalic_latitude(radians(10.0));
    if (!beta.ok()) {
        std::cerr << "camera diagnostic could not derive authalic latitude\n";
        return;
    }
    const aeris::geo::Mat3 world_to_view = aeris::geo::multiply(
        aeris::geo::rotation_y(beta.value),
        aeris::geo::rotation_z(-radians(45.0))
    );

    const std::string wanted = "ne_110m_land:v5.1.2:record:111";
    for (const auto& feature : world.features) {
        if (feature.stable_id != wanted) {
            continue;
        }

        for (std::size_t ring_index = 0U; ring_index < feature.rings.size(); ++ring_index) {
            aeris::view::GlobePolygonOptions options{};
            options.curve.geometric_tolerance_m = 5'000.0;
            options.curve.horizon_tolerance_m = 0.01;
            options.curve.max_subdivision_depth = 32U;
            options.curve.max_root_iterations = 80U;
            options.curve.max_segments = 1'000'000U;
            options.horizon_arc_tolerance_m = 500.0;
            options.max_horizon_arc_segments = 1'000'000U;
            options.max_output_rings = 4096U;

            std::cerr << "camera45/10 diagnostic " << wanted
                      << " ring=" << ring_index
                      << " interior=" << static_cast<unsigned>(feature.rings[ring_index].geometry.interior_side)
                      << '\n';

            for (unsigned round = 1U; round <= 22U; ++round) {
                const auto finite = aeris::view::project_visible_wgs84_linear_polygon_ring(
                    feature.rings[ring_index].geometry,
                    world_to_view,
                    options,
                    aeris::geo::authalic_radius_m()
                );
                std::cerr
                    << "  round=" << round
                    << " curve_tol=" << options.curve.geometric_tolerance_m
                    << " arc_tol=" << options.horizon_arc_tolerance_m
                    << " error=" << static_cast<unsigned>(finite.error)
                    << " curve_error=" << static_cast<unsigned>(finite.curve_error)
                    << " crossings=" << finite.horizon_crossings
                    << " rings=" << finite.rings.size()
                    << " planar_area=" << finite.planar_signed_area_m2
                    << " component_areas=";
                for (const auto& ring : finite.rings) {
                    std::cerr << ' ' << aeris::geometry::signed_planar_area(ring);
                }
                std::cerr << '\n';

                options.curve.geometric_tolerance_m *= 0.5;
                options.horizon_arc_tolerance_m *= 0.5;
            }
        }
        return;
    }
    std::cerr << "camera diagnostic could not find " << wanted << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    std::filesystem::path snapshot =
        std::filesystem::path("dev-data") / "natural-earth-v5.1.2";
    if (argc == 3 && std::string(argv[1]) == "--snapshot") {
        snapshot = argv[2];
    } else if (argc != 1) {
        std::cerr << "usage: aeris_viewer_scene_probe [--snapshot <directory>]\n";
        return EXIT_FAILURE;
    }

    auto loaded = aeris::viewer::load_pinned_demo_world(
        snapshot,
        "viewer-scene-probe"
    );
    if (!loaded.ok()) {
        std::cerr << "scene probe source load failed: "
                  << loaded.diagnostic << '\n';
        return EXIT_FAILURE;
    }

    constexpr std::array<aeris::viewer::ViewMode, 3U> modes{
        aeris::viewer::ViewMode::globe,
        aeris::viewer::ViewMode::sinusoidal,
        aeris::viewer::ViewMode::mollweide,
    };

    for (const auto mode : modes) {
        aeris::viewer::SceneRequest request{};
        request.mode = mode;
        request.quality = aeris::viewer::SceneQuality::verified;
        request.camera_longitude_deg = 15.0;
        request.camera_latitude_deg = 20.0;

        const aeris::viewer::SceneData scene =
            aeris::viewer::build_scene(*loaded.world, request);
        if (!scene_has_expected_geometry(scene)) {
            print_scene_failure(aeris::viewer::view_mode_name(mode), scene);
            return EXIT_FAILURE;
        }

        std::cout
            << aeris::viewer::view_mode_name(mode)
            << ": features=" << scene.features.size()
            << " fill_rings=" << scene.fill_rings
            << " outlines=" << scene.outline_parts
            << " vertices=" << scene.vertices
            << " max_refinement=" << scene.max_refinement_rounds
            << '\n';
    }

    aeris::viewer::SceneRequest camera_regression{};
    camera_regression.mode = aeris::viewer::ViewMode::globe;
    camera_regression.quality = aeris::viewer::SceneQuality::verified;
    camera_regression.camera_longitude_deg = 45.0;
    camera_regression.camera_latitude_deg = 10.0;
    const aeris::viewer::SceneData camera_scene =
        aeris::viewer::build_scene(*loaded.world, camera_regression);
    if (!scene_has_expected_geometry(camera_scene)) {
        print_scene_failure("Globe camera regression 45E/10N", camera_scene);
        diagnose_camera_record_111(*loaded.world);
        return EXIT_FAILURE;
    }
    std::cout << "Globe camera regression 45E/10N: PASS"
              << " fill_rings=" << camera_scene.fill_rings
              << " outlines=" << camera_scene.outline_parts
              << " vertices=" << camera_scene.vertices
              << " max_refinement=" << camera_scene.max_refinement_rounds
              << '\n';

    const auto unfold = aeris::viewer::build_unfold_bundle(
        *loaded.world,
        15.0,
        20.0,
        aeris::viewer::ViewMode::mollweide
    );
    if (!unfold_has_expected_contract(unfold)) {
        std::cerr << "unfold contract probe failed: ok=" << unfold.ok
                  << " canceled=" << unfold.canceled
                  << " diagnostic=" << unfold.diagnostic
                  << " guides=" << unfold.guides.size() << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "Unfold: guides=" << unfold.guides.size()
              << " globe_vertices=" << unfold.globe_endpoint.vertices
              << " flat_vertices=" << unfold.flat_endpoint.vertices << '\n';

    std::cout << "viewer_scene_probe: PASS\n";
    return EXIT_SUCCESS;
}
