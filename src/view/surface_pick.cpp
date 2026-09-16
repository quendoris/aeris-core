// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/view/surface.hpp"

#include "aeris/geo/rotation.hpp"
#include "aeris/geo/wgs84.hpp"
#include "aeris/projection/sinu_mollweide.hpp"

#include <algorithm>
#include <cmath>

namespace aeris::view {
namespace {

constexpr double kDegreesToRadians = geo::kPi / 180.0;
constexpr double kRadiansToDegrees = 180.0 / geo::kPi;
constexpr double kTwoPi = 2.0 * geo::kPi;

[[nodiscard]] geo::Mat3 world_to_projection_frame(const SurfaceMode mode) noexcept {
    if (mode == SurfaceMode::sinu_mollweide) {
        return projection::philbrick_world_to_projection_matrix();
    }
    return {};
}

}  // namespace

ProjectionCutPickResult pick_projection_cut_from_globe(
    const SurfaceMode mode,
    const double camera_longitude_deg,
    const double camera_latitude_deg,
    const geometry::PlanarPoint globe_point
) {
    ProjectionCutPickResult result{};

    if (mode == SurfaceMode::globe) {
        result.ok = false;
        result.diagnostic = "direct movable cut picking requires a planar target surface";
        return result;
    }
    if (!std::isfinite(camera_longitude_deg) ||
        !std::isfinite(camera_latitude_deg) ||
        camera_latitude_deg < -90.0 || camera_latitude_deg > 90.0 ||
        !std::isfinite(globe_point.x) || !std::isfinite(globe_point.y)) {
        result.ok = false;
        result.diagnostic = "projection cut pick contains non-finite or invalid coordinates";
        return result;
    }

    const double radius_m = geo::authalic_radius_m();
    const double view_y = globe_point.x / radius_m;
    const double view_z = globe_point.y / radius_m;
    const double radial2 = view_y * view_y + view_z * view_z;
    if (radial2 > 1.0 + 1e-12) {
        result.ok = false;
        result.diagnostic = "projection cut pick lies outside visible Globe disk";
        return result;
    }

    const double view_x = std::sqrt(std::max(0.0, 1.0 - radial2));
    const geo::Vec3 camera{view_x, view_y, view_z};

    const geo::ScalarResult camera_beta = geo::authalic_latitude(
        camera_latitude_deg * kDegreesToRadians
    );
    if (!camera_beta.ok()) {
        result.ok = false;
        result.diagnostic = "unable to derive authalic camera latitude for cut pick";
        return result;
    }

    const double camera_longitude_rad = std::remainder(
        camera_longitude_deg * kDegreesToRadians,
        kTwoPi
    );
    const geo::Mat3 world_to_view = geo::multiply(
        geo::rotation_y(camera_beta.value),
        geo::rotation_z(-camera_longitude_rad)
    );
    const geo::Mat3 projection_frame = world_to_projection_frame(mode);
    if (!geo::is_rotation_matrix(world_to_view) ||
        !geo::is_rotation_matrix(projection_frame)) {
        result.ok = false;
        result.diagnostic = "projection cut pick frame is not a valid rotation";
        return result;
    }

    const geo::Vec3 world = geo::apply(geo::transpose(world_to_view), camera);
    const geo::Vec3 framed_vector = geo::apply(projection_frame, world);
    const geo::LonLatResult framed = geo::unit_vector_to_lonlat(framed_vector);
    if (!framed.ok()) {
        result.ok = false;
        result.diagnostic = "projection cut pick is indeterminate at a projection-frame pole";
        return result;
    }

    const double central_meridian_rad = std::remainder(
        framed.value.longitude_rad - geo::kPi,
        kTwoPi
    );
    result.projection_central_meridian_deg =
        central_meridian_rad * kRadiansToDegrees;
    result.diagnostic = mode == SurfaceMode::sinu_mollweide
        ? "visible Globe point converted to Philbrick projection cut"
        : "visible Globe point converted to projection-frame cut";
    return result;
}

}  // namespace aeris::view
