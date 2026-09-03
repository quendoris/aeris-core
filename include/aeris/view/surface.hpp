// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "aeris/geometry/planar.hpp"
#include "aeris/view/scene.hpp"

#include <string>
#include <vector>

namespace aeris::view {

// Geometry of the mathematical planar surface itself, independent of any
// dataset rendered onto it. Frontends use this for viewport fitting, ocean or
// sheet presentation, and unfold guides without deriving projection math or
// duplicating the same outline in every source scene cache.
struct PlanarSurfaceGeometry final {
    SurfaceMode mode = SurfaceMode::sinu_mollweide;
    std::vector<geometry::PlanarPoint> outline;

    double min_x = 0.0;
    double min_y = 0.0;
    double max_x = 0.0;
    double max_y = 0.0;

    bool ok = true;
    std::string diagnostic;
};

// Build the complete authalic-radius domain boundary for a planar equal-area
// AERIS surface. The central-meridian choice moves which world geometry reaches
// the two cut edges; it does not change the planar sheet envelope itself.
[[nodiscard]] PlanarSurfaceGeometry build_planar_surface_geometry(
    SurfaceMode mode
);

}  // namespace aeris::view
