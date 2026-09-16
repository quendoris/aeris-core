// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "aeris/geometry/planar.hpp"
#include "aeris/view/scene.hpp"

#include <string>

namespace aeris::view {

struct SurfaceGeographicPickResult final {
    double longitude_deg{0.0};
    double latitude_deg{0.0};
    bool longitude_indeterminate{false};
    bool ok{false};
    std::string diagnostic;
};

// Inverts one point in canonical surface metres back to WGS84 geography. Globe
// inversion uses the displayed camera and the visible hemisphere. Flat surfaces
// use the projection central meridian/cut carried by the verified scene. This is
// a render-neutral geographic primitive for cursor readout, raster/grid sampling
// and surface-to-surface focus continuity.
[[nodiscard]] SurfaceGeographicPickResult pick_geographic_from_surface(
    SurfaceMode mode,
    geometry::PlanarPoint surface_point,
    double camera_longitude_deg,
    double camera_latitude_deg,
    double projection_central_meridian_deg);

}  // namespace aeris::view
