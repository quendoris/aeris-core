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

// One physical sample of the projection cut. The left and right planar points
// are two sheet-side representations of the same point on the folded Globe.
// This is deliberately enough information for both a live seam overlay and a
// later non-normative unfold interpolation without making a frontend reproduce
// Philbrick-frame mathematics.
struct ProjectionSeamSample final {
    geometry::PlanarPoint globe{};
    geometry::PlanarPoint flat_left{};
    geometry::PlanarPoint flat_right{};
    double globe_depth_normalized = 0.0;
    bool globe_visible = false;
};

struct ProjectionSeamGeometry final {
    SurfaceMode mode = SurfaceMode::sinu_mollweide;
    double camera_longitude_deg = 15.0;
    double camera_latitude_deg = 20.0;
    double projection_central_meridian_deg = 0.0;
    std::vector<ProjectionSeamSample> samples;

    bool ok = true;
    std::string diagnostic;
};

// Build the complete authalic-radius domain boundary for a planar equal-area
// AERIS surface. The central-meridian choice moves which world geometry reaches
// the two cut edges; it does not change the planar sheet envelope itself.
[[nodiscard]] PlanarSurfaceGeometry build_planar_surface_geometry(
    SurfaceMode mode
);

// Build the physical pole-to-pole projection cut as seen by the current Globe
// camera together with its two planar boundary locations. For the oblique
// Sinu-Mollweide surface, projection_central_meridian_deg belongs to the
// Philbrick frame; moving it rotates the physical cut without changing the
// planar sheet itself.
[[nodiscard]] ProjectionSeamGeometry build_projection_seam_geometry(
    SurfaceMode mode,
    double camera_longitude_deg,
    double camera_latitude_deg,
    double projection_central_meridian_deg
);

}  // namespace aeris::view
