// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "aeris/geometry/geographic.hpp"
#include "aeris/geometry/planar.hpp"
#include "aeris/view/globe.hpp"

#include <cstddef>
#include <vector>

namespace aeris::view {

enum class GlobeCurveError {
    none = 0,
    invalid_options,
    invalid_geometry,
    sample_failed,
    horizon_non_convergence,
    limit_exceeded,
};

struct GlobeCurveOptions final {
    double geometric_tolerance_m = 100.0;
    double horizon_tolerance_m = 0.001;
    unsigned max_subdivision_depth = 32U;
    unsigned max_root_iterations = 80U;
    std::size_t max_segments = 1'000'000U;
};

struct GlobeCurveResult final {
    std::vector<std::vector<geometry::PlanarPoint>> visible_parts;

    std::size_t horizon_crossings = 0U;
    std::size_t projected_vertices = 0U;
    unsigned deepest_subdivision_level = 0U;

    GlobeCurveError error = GlobeCurveError::none;
    geo::MathError sample_error = geo::MathError::none;

    [[nodiscard]] bool ok() const noexcept {
        return error == GlobeCurveError::none;
    }
};

// Project one canonical wgs84-linear-v1 edge to the visible hemisphere of an
// orthographic authalic globe. The input longitudes may be unwrapped. Returned
// parts contain only visible geometry; every visible/hidden transition is
// terminated at a numerically solved depth(t)=0 horizon point.
[[nodiscard]] GlobeCurveResult project_visible_wgs84_linear_edge(
    geometry::GeodeticPoint start,
    geometry::GeodeticPoint end,
    const geo::Mat3& world_to_view,
    const GlobeCurveOptions& options = {},
    double radius_m = geo::authalic_radius_m()
);

// Apply the same edge contract to a complete canonical ring and merge visible
// fragments that remain continuous through adjacent source vertices. No
// artificial chord is added across the hidden hemisphere or along the limb.
[[nodiscard]] GlobeCurveResult project_visible_wgs84_linear_ring(
    const geometry::LinearRing& ring,
    const geo::Mat3& world_to_view,
    const GlobeCurveOptions& options = {},
    double radius_m = geo::authalic_radius_m()
);

}  // namespace aeris::view
