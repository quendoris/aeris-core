// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/view/surface.hpp"

#include "aeris/geo/rotation.hpp"
#include "aeris/geo/wgs84.hpp"
#include "aeris/projection/primitives.hpp"
#include "aeris/projection/sinu_mollweide.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace aeris::view {
namespace {

constexpr double kDegreesToRadians = geo::kPi / 180.0;
constexpr double kTwoPi = 2.0 * geo::kPi;

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

[[nodiscard]] std::vector<double> surface_latitudes(const SurfaceMode mode) {
    std::vector<double> latitudes;
    latitudes.reserve(182U);
    for (int latitude_deg = -90; latitude_deg <= 90; ++latitude_deg) {
        latitudes.push_back(static_cast<double>(latitude_deg) * kDegreesToRadians);
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
    return latitudes;
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

[[nodiscard]] bool finite_seam_request(
    const double camera_longitude_deg,
    const double camera_latitude_deg,
    const double projection_central_meridian_deg
) noexcept {
    return std::isfinite(camera_longitude_deg) &&
        std::isfinite(camera_latitude_deg) &&
        camera_latitude_deg >= -90.0 &&
        camera_latitude_deg <= 90.0 &&
        std::isfinite(projection_central_meridian_deg);
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

    const std::vector<double> latitudes = surface_latitudes(mode);

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

ProjectionSeamGeometry build_projection_seam_geometry(
    const SurfaceMode mode,
    const double camera_longitude_deg,
    const double camera_latitude_deg,
    const double projection_central_meridian_deg
) {
    ProjectionSeamGeometry seam{};
    seam.mode = mode;
    seam.camera_longitude_deg = camera_longitude_deg;
    seam.camera_latitude_deg = camera_latitude_deg;
    seam.projection_central_meridian_deg = projection_central_meridian_deg;

    if (mode == SurfaceMode::globe) {
        seam.ok = false;
        seam.diagnostic = "projection seam requires a planar target surface";
        return seam;
    }
    if (!finite_seam_request(
            camera_longitude_deg,
            camera_latitude_deg,
            projection_central_meridian_deg
        )) {
        seam.ok = false;
        seam.diagnostic = "projection seam request contains invalid camera or cut coordinates";
        return seam;
    }

    const geo::ScalarResult camera_beta = geo::authalic_latitude(
        camera_latitude_deg * kDegreesToRadians
    );
    if (!camera_beta.ok()) {
        seam.ok = false;
        seam.diagnostic = "unable to derive authalic camera latitude for projection seam";
        return seam;
    }

    const double camera_longitude_rad = std::remainder(
        camera_longitude_deg * kDegreesToRadians,
        kTwoPi
    );
    const geo::Mat3 world_to_view = geo::multiply(
        geo::rotation_y(camera_beta.value),
        geo::rotation_z(-camera_longitude_rad)
    );

    geo::Mat3 projection_to_world{};
    if (mode == SurfaceMode::sinu_mollweide) {
        projection_to_world = geo::transpose(
            projection::philbrick_world_to_projection_matrix()
        );
    }
    if (!geo::is_rotation_matrix(projection_to_world) ||
        !geo::is_rotation_matrix(world_to_view)) {
        seam.ok = false;
        seam.diagnostic = "projection seam frame is not a valid rotation";
        return seam;
    }

    const double cut_longitude_rad = std::remainder(
        projection_central_meridian_deg * kDegreesToRadians + geo::kPi,
        kTwoPi
    );
    const double radius_m = geo::authalic_radius_m();
    const std::vector<double> latitudes = surface_latitudes(mode);
    seam.samples.reserve(latitudes.size());

    for (const double latitude : latitudes) {
        const geo::Vec3 projection_frame = geo::lonlat_to_unit_vector(
            cut_longitude_rad,
            latitude
        );
        const geo::Vec3 world = geo::apply(projection_to_world, projection_frame);
        const geo::Vec3 camera = geo::apply(world_to_view, world);
        if (!std::isfinite(camera.x) ||
            !std::isfinite(camera.y) ||
            !std::isfinite(camera.z)) {
            seam.ok = false;
            seam.samples.clear();
            seam.diagnostic = "projection seam produced a non-finite Globe sample";
            return seam;
        }

        const projection::PlanarResult flat_left = project_boundary_point(
            mode,
            -geo::kPi,
            latitude,
            radius_m
        );
        const projection::PlanarResult flat_right = project_boundary_point(
            mode,
            geo::kPi,
            latitude,
            radius_m
        );
        if (!flat_left.ok() || !flat_right.ok() ||
            !std::isfinite(flat_left.value.x) ||
            !std::isfinite(flat_left.value.y) ||
            !std::isfinite(flat_right.value.x) ||
            !std::isfinite(flat_right.value.y)) {
            seam.ok = false;
            seam.samples.clear();
            seam.diagnostic = "projection seam produced a non-finite planar boundary sample";
            return seam;
        }

        seam.samples.push_back({
            {radius_m * camera.y, radius_m * camera.z},
            flat_left.value,
            flat_right.value,
            camera.x,
            camera.x >= 0.0,
        });
    }

    if (seam.samples.size() < 3U) {
        seam.ok = false;
        seam.samples.clear();
        seam.diagnostic = "projection seam produced too few samples";
        return seam;
    }

    seam.diagnostic = mode == SurfaceMode::sinu_mollweide
        ? "Philbrick projection-frame cut with coincident folded sides"
        : "projection-frame cut with coincident folded sides";
    return seam;
}

}  // namespace aeris::view
