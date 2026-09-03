// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/view/surface.hpp"

#include "aeris/geo/wgs84.hpp"
#include "aeris/projection/primitives.hpp"
#include "aeris/projection/sinu_mollweide.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace aeris::view {
namespace {

[[nodiscard]] projection::PlanarResult project_boundary_point(
    const SurfaceMode mode,
    const double longitude_delta_rad,
    const double authalic_latitude_rad,
    const double radius_m
) noexcept {
    switch (mode) {
    case SurfaceMode::sinusoidal:
        return projection::sinusoidal_forward(
            longitude_delta_rad,
            authalic_latitude_rad,
            radius_m
        );
    case SurfaceMode::mollweide:
        return projection::mollweide_forward(
            longitude_delta_rad,
            authalic_latitude_rad,
            radius_m
        );
    case SurfaceMode::sinu_mollweide:
        return projection::sinu_mollweide_forward(
            longitude_delta_rad,
            authalic_latitude_rad,
            radius_m
        );
    case SurfaceMode::globe:
        break;
    }
    return {{}, geo::MathError::numerical_domain_error};
}

void include_point(
    PlanarSurfaceGeometry& surface,
    const geometry::PlanarPoint point
) noexcept {
    surface.min_x = std::min(surface.min_x, point.x);
    surface.min_y = std::min(surface.min_y, point.y);
    surface.max_x = std::max(surface.max_x, point.x);
    surface.max_y = std::max(surface.max_y, point.y);
}

[[nodiscard]] bool append_boundary_point(
    PlanarSurfaceGeometry& surface,
    const double longitude_delta_rad,
    const double authalic_latitude_rad,
    const double radius_m
) {
    const projection::PlanarResult projected = project_boundary_point(
        surface.mode,
        longitude_delta_rad,
        authalic_latitude_rad,
        radius_m
    );
    if (!projected.ok() ||
        !std::isfinite(projected.value.x) ||
        !std::isfinite(projected.value.y)) {
        return false;
    }
    surface.outline.push_back(projected.value);
    include_point(surface, projected.value);
    return true;
}

}  // namespace

PlanarSurfaceGeometry build_planar_surface_geometry(const SurfaceMode mode) {
    PlanarSurfaceGeometry surface{};
    surface.mode = mode;
    if (mode == SurfaceMode::globe) {
        surface.ok = false;
        surface.diagnostic = "Globe has no planar surface envelope";
        return surface;
    }

    std::vector<double> latitudes;
    latitudes.reserve(182U);
    constexpr double degree = geo::kPi / 180.0;
    for (int latitude_deg = -90; latitude_deg <= 90; ++latitude_deg) {
        latitudes.push_back(static_cast<double>(latitude_deg) * degree);
    }
    if (mode == SurfaceMode::sinu_mollweide) {
        latitudes.push_back(-projection::kSinuMollweideTransitionLatitudeRad);
        std::sort(latitudes.begin(), latitudes.end());
        latitudes.erase(
            std::unique(
                latitudes.begin(),
                latitudes.end(),
                [](const double left, const double right) {
                    return std::abs(left - right) <= 1e-14;
                }
            ),
            latitudes.end()
        );
    }

    const double infinity = std::numeric_limits<double>::infinity();
    surface.min_x = infinity;
    surface.min_y = infinity;
    surface.max_x = -infinity;
    surface.max_y = -infinity;
    surface.outline.reserve(latitudes.size() * 2U);

    const double radius_m = geo::authalic_radius_m();
    for (const double latitude : latitudes) {
        if (!append_boundary_point(
                surface,
                geo::kPi,
                latitude,
                radius_m
            )) {
            surface.ok = false;
            surface.diagnostic = "unable to project right planar surface boundary";
            return surface;
        }
    }

    // The north/south poles are common to both seam sides. Omit duplicate pole
    // vertices here; a frontend closes the polygon from the final left-side
    // sample back to the first south-pole sample.
    if (latitudes.size() > 2U) {
        for (std::size_t index = latitudes.size() - 1U; index-- > 1U;) {
            if (!append_boundary_point(
                    surface,
                    -geo::kPi,
                    latitudes[index],
                    radius_m
                )) {
                surface.ok = false;
                surface.diagnostic = "unable to project left planar surface boundary";
                return surface;
            }
        }
    }

    if (surface.outline.size() < 4U ||
        !std::isfinite(surface.min_x) ||
        !std::isfinite(surface.min_y) ||
        !std::isfinite(surface.max_x) ||
        !std::isfinite(surface.max_y) ||
        surface.max_x <= surface.min_x ||
        surface.max_y <= surface.min_y) {
        surface.ok = false;
        surface.diagnostic = "planar surface envelope produced invalid geometry";
        return surface;
    }

    switch (mode) {
    case SurfaceMode::sinusoidal:
        surface.diagnostic = "authalic Sinusoidal surface envelope";
        break;
    case SurfaceMode::mollweide:
        surface.diagnostic = "authalic Mollweide surface envelope";
        break;
    case SurfaceMode::sinu_mollweide:
        surface.diagnostic = "authalic Philbrick Sinu-Mollweide surface envelope";
        break;
    case SurfaceMode::globe:
        break;
    }
    return surface;
}

}  // namespace aeris::view
