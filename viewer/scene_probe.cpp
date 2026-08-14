// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "scene_builder.hpp"
#include "world_loader.hpp"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

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
            std::cerr
                << "scene probe failed for "
                << aeris::viewer::view_mode_name(mode)
                << ": ok=" << scene.ok
                << " diagnostic=" << scene.diagnostic
                << " features=" << scene.features.size()
                << " fill_rings=" << scene.fill_rings
                << " outlines=" << scene.outline_parts
                << " vertices=" << scene.vertices
                << " max_refinement=" << scene.max_refinement_rounds
                << '\n';
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

    std::cout << "viewer_scene_probe: PASS\n";
    return EXIT_SUCCESS;
}
