// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "aeris/geometry/planar.hpp"
#include "aeris/source/adapter.hpp"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace aeris::view {

enum class SurfaceMode {
    globe = 0,
    sinusoidal,
    mollweide,
    sinu_mollweide,
};

enum class SceneQuality {
    preview = 0,
    verified,
};

struct SceneFeatureGeometry final {
    std::string stable_id;
    std::vector<std::vector<geometry::PlanarPoint>> fill_rings;
    std::vector<std::vector<geometry::PlanarPoint>> outlines;
};

struct SceneGeometry final {
    SurfaceMode mode = SurfaceMode::globe;
    SceneQuality quality = SceneQuality::preview;
    std::vector<SceneFeatureGeometry> features;

    double min_x = -1.0;
    double min_y = -1.0;
    double max_x = 1.0;
    double max_y = 1.0;
    double globe_radius_m = 0.0;

    double camera_longitude_deg = 15.0;
    double camera_latitude_deg = 20.0;

    // Independent of the Globe camera. On the combined Sinu-Mollweide surface
    // this is the central meridian of the Philbrick projection frame; its
    // opposite meridian is the one physical cut. Frontends may therefore move
    // the cut without pretending to pan or rotate the viewer camera.
    double projection_central_meridian_deg = 0.0;

    std::size_t fill_rings = 0U;
    std::size_t outline_parts = 0U;
    std::size_t vertices = 0U;
    unsigned max_refinement_rounds = 0U;

    bool canceled = false;
    bool ok = true;
    std::string diagnostic;
};

struct SceneRequest final {
    SurfaceMode mode = SurfaceMode::globe;
    SceneQuality quality = SceneQuality::verified;
    double camera_longitude_deg = 15.0;
    double camera_latitude_deg = 20.0;
    double projection_central_meridian_deg = 0.0;
};

using CancelCheck = std::function<bool()>;

// Build render-neutral geometry from one already verified canonical source.
// Stable feature identity is copied while geometry is constructed; presentation
// metadata, layer meaning, styling, labels, Qt types and project storage are
// deliberately outside this boundary.
[[nodiscard]] SceneGeometry build_scene_geometry(
    const source::Result& world,
    const SceneRequest& request,
    const CancelCheck& canceled = {}
);

[[nodiscard]] const char* surface_mode_name(SurfaceMode mode) noexcept;

}  // namespace aeris::view
