// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "aeris/geometry/planar.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace aeris::viewer {

enum class ViewMode {
    globe = 0,
    sinusoidal,
    mollweide,
};

enum class SceneQuality {
    preview = 0,
    verified,
};

struct SceneFeature final {
    std::string stable_id;
    std::string style_key;
    std::string label;
    std::string iso_a2;
    std::vector<std::vector<geometry::PlanarPoint>> fill_rings;
    std::vector<std::vector<geometry::PlanarPoint>> outlines;
};

struct SceneData final {
    ViewMode mode = ViewMode::globe;
    SceneQuality quality = SceneQuality::preview;
    bool political = false;
    std::vector<SceneFeature> features;

    double min_x = -1.0;
    double min_y = -1.0;
    double max_x = 1.0;
    double max_y = 1.0;
    double globe_radius_m = 0.0;

    double camera_longitude_deg = 15.0;
    double camera_latitude_deg = 20.0;

    std::size_t fill_rings = 0U;
    std::size_t outline_parts = 0U;
    std::size_t vertices = 0U;
    unsigned max_refinement_rounds = 0U;

    bool canceled = false;
    bool ok = true;
    std::string diagnostic;
};

struct SceneRequest final {
    ViewMode mode = ViewMode::globe;
    SceneQuality quality = SceneQuality::verified;
    double camera_longitude_deg = 15.0;
    double camera_latitude_deg = 20.0;
};

[[nodiscard]] const char* view_mode_name(ViewMode mode) noexcept;

}  // namespace aeris::viewer
