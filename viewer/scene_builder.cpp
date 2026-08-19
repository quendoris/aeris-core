// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "scene_builder.hpp"

#include "aeris/view/scene.hpp"

#include <utility>

namespace aeris::viewer {
namespace {

[[nodiscard]] aeris::view::SurfaceMode to_core_mode(const ViewMode mode) noexcept {
    switch (mode) {
    case ViewMode::globe:
        return aeris::view::SurfaceMode::globe;
    case ViewMode::sinusoidal:
        return aeris::view::SurfaceMode::sinusoidal;
    case ViewMode::mollweide:
        return aeris::view::SurfaceMode::mollweide;
    }
    return aeris::view::SurfaceMode::globe;
}

[[nodiscard]] aeris::view::SceneQuality to_core_quality(
    const SceneQuality quality
) noexcept {
    return quality == SceneQuality::verified
        ? aeris::view::SceneQuality::verified
        : aeris::view::SceneQuality::preview;
}

[[nodiscard]] SceneData from_core_scene(aeris::view::SceneGeometry core) {
    SceneData scene{};
    switch (core.mode) {
    case aeris::view::SurfaceMode::globe:
        scene.mode = ViewMode::globe;
        break;
    case aeris::view::SurfaceMode::sinusoidal:
        scene.mode = ViewMode::sinusoidal;
        break;
    case aeris::view::SurfaceMode::mollweide:
        scene.mode = ViewMode::mollweide;
        break;
    }
    scene.quality = core.quality == aeris::view::SceneQuality::verified
        ? SceneQuality::verified
        : SceneQuality::preview;

    scene.features.reserve(core.features.size());
    for (auto& input : core.features) {
        SceneFeature output{};
        output.stable_id = std::move(input.stable_id);
        output.fill_rings = std::move(input.fill_rings);
        output.outlines = std::move(input.outlines);
        scene.features.push_back(std::move(output));
    }

    scene.min_x = core.min_x;
    scene.min_y = core.min_y;
    scene.max_x = core.max_x;
    scene.max_y = core.max_y;
    scene.globe_radius_m = core.globe_radius_m;
    scene.camera_longitude_deg = core.camera_longitude_deg;
    scene.camera_latitude_deg = core.camera_latitude_deg;
    scene.fill_rings = core.fill_rings;
    scene.outline_parts = core.outline_parts;
    scene.vertices = core.vertices;
    scene.max_refinement_rounds = core.max_refinement_rounds;
    scene.canceled = core.canceled;
    scene.ok = core.ok;
    scene.diagnostic = std::move(core.diagnostic);
    return scene;
}

}  // namespace

const char* view_mode_name(const ViewMode mode) noexcept {
    switch (mode) {
    case ViewMode::globe:
        return "Globe";
    case ViewMode::sinusoidal:
        return "Sinusoidal";
    case ViewMode::mollweide:
        return "Mollweide";
    }
    return "Unknown";
}

SceneData build_scene(
    const source::Result& world,
    const SceneRequest& request,
    const CancelCheck& canceled
) {
    aeris::view::SceneRequest core_request{};
    core_request.mode = to_core_mode(request.mode);
    core_request.quality = to_core_quality(request.quality);
    core_request.camera_longitude_deg = request.camera_longitude_deg;
    core_request.camera_latitude_deg = request.camera_latitude_deg;
    return from_core_scene(
        aeris::view::build_scene_geometry(world, core_request, canceled)
    );
}

}  // namespace aeris::viewer
