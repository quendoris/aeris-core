// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "unfold.hpp"

#include "aeris/geo/rotation.hpp"
#include "aeris/geo/wgs84.hpp"
#include "aeris/projection/primitives.hpp"
#include "aeris/view/globe.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace aeris::viewer {
namespace {

[[nodiscard]] double radians(const double degrees) noexcept {
    return degrees * geo::kPi / 180.0;
}

[[nodiscard]] bool should_cancel(const CancelCheck& canceled) {
    return static_cast<bool>(canceled) && canceled();
}

[[nodiscard]] bool append_guide_vertex(
    UnfoldGuideLine& line,
    const double longitude_deg,
    const double geodetic_latitude_deg,
    const ViewMode target_mode,
    const geo::Mat3& world_to_view,
    const double radius_m
) {
    const double longitude = radians(longitude_deg);
    const auto beta = geo::authalic_latitude(radians(geodetic_latitude_deg));
    if (!beta.ok()) {
        return false;
    }

    const auto globe = view::orthographic_globe_point(
        longitude,
        beta.value,
        world_to_view,
        radius_m
    );
    if (!globe.ok()) {
        return false;
    }

    const projection::PlanarResult flat = target_mode == ViewMode::sinusoidal
        ? projection::sinusoidal_forward(longitude, beta.value, radius_m)
        : projection::mollweide_forward(longitude, beta.value, radius_m);
    if (!flat.ok()) {
        return false;
    }

    line.vertices.push_back({
        {globe.value.x, globe.value.y},
        flat.value,
        globe.value.depth / radius_m,
    });
    return true;
}

[[nodiscard]] bool append_meridian(
    std::vector<UnfoldGuideLine>& guides,
    const double longitude_deg,
    const UnfoldGuideKind kind,
    const ViewMode target_mode,
    const geo::Mat3& world_to_view,
    const double radius_m,
    const CancelCheck& canceled
) {
    UnfoldGuideLine line{};
    line.kind = kind;
    line.vertices.reserve(37U);
    for (int latitude_deg = -90; latitude_deg <= 90; latitude_deg += 5) {
        if (should_cancel(canceled)) {
            return false;
        }
        if (!append_guide_vertex(
                line,
                longitude_deg,
                static_cast<double>(latitude_deg),
                target_mode,
                world_to_view,
                radius_m)) {
            return false;
        }
    }
    guides.push_back(std::move(line));
    return true;
}

[[nodiscard]] bool append_parallel(
    std::vector<UnfoldGuideLine>& guides,
    const double latitude_deg,
    const ViewMode target_mode,
    const geo::Mat3& world_to_view,
    const double radius_m,
    const CancelCheck& canceled
) {
    UnfoldGuideLine line{};
    line.kind = UnfoldGuideKind::graticule;
    line.vertices.reserve(73U);
    for (int longitude_deg = -180; longitude_deg <= 180; longitude_deg += 5) {
        if (should_cancel(canceled)) {
            return false;
        }
        if (!append_guide_vertex(
                line,
                static_cast<double>(longitude_deg),
                latitude_deg,
                target_mode,
                world_to_view,
                radius_m)) {
            return false;
        }
    }
    guides.push_back(std::move(line));
    return true;
}

}  // namespace

double unfold_eased_progress(const double progress) noexcept {
    const double p = std::clamp(progress, 0.0, 1.0);
    return p * p * p * (p * (p * 6.0 - 15.0) + 10.0);
}

geometry::PlanarPoint interpolate_unfold_vertex(
    const UnfoldGuideVertex& vertex,
    const double progress
) noexcept {
    const double t = unfold_eased_progress(progress);
    return {
        vertex.globe.x + (vertex.flat.x - vertex.globe.x) * t,
        vertex.globe.y + (vertex.flat.y - vertex.globe.y) * t,
    };
}

double unfold_guide_visibility(
    const UnfoldGuideVertex& vertex,
    const double progress
) noexcept {
    if (vertex.globe_depth_normalized >= 0.0) {
        return 1.0;
    }
    return unfold_eased_progress(progress);
}

UnfoldBundle build_unfold_bundle(
    const source::Result& world,
    const double camera_longitude_deg,
    const double camera_latitude_deg,
    const ViewMode target_mode,
    const CancelCheck& canceled
) {
    UnfoldBundle bundle{};
    bundle.target_mode = target_mode;

    if (target_mode == ViewMode::globe) {
        bundle.ok = false;
        bundle.diagnostic = "unfold target must be a planar view";
        return bundle;
    }
    if (should_cancel(canceled)) {
        bundle.canceled = true;
        return bundle;
    }

    SceneRequest globe_request{};
    globe_request.mode = ViewMode::globe;
    globe_request.quality = SceneQuality::verified;
    globe_request.camera_longitude_deg = camera_longitude_deg;
    globe_request.camera_latitude_deg = camera_latitude_deg;
    bundle.globe_endpoint = build_scene(world, globe_request, canceled);
    if (bundle.globe_endpoint.canceled || should_cancel(canceled)) {
        bundle.canceled = true;
        return bundle;
    }
    if (!bundle.globe_endpoint.ok ||
        bundle.globe_endpoint.quality != SceneQuality::verified) {
        bundle.ok = false;
        bundle.diagnostic = "unable to build verified globe endpoint: " +
            bundle.globe_endpoint.diagnostic;
        return bundle;
    }

    SceneRequest flat_request{};
    flat_request.mode = target_mode;
    flat_request.quality = SceneQuality::verified;
    flat_request.camera_longitude_deg = camera_longitude_deg;
    flat_request.camera_latitude_deg = camera_latitude_deg;
    bundle.flat_endpoint = build_scene(world, flat_request, canceled);
    if (bundle.flat_endpoint.canceled || should_cancel(canceled)) {
        bundle.canceled = true;
        return bundle;
    }
    if (!bundle.flat_endpoint.ok ||
        bundle.flat_endpoint.quality != SceneQuality::verified) {
        bundle.ok = false;
        bundle.diagnostic = "unable to build verified planar endpoint: " +
            bundle.flat_endpoint.diagnostic;
        return bundle;
    }

    const auto beta = geo::authalic_latitude(radians(camera_latitude_deg));
    if (!beta.ok()) {
        bundle.ok = false;
        bundle.diagnostic = "unable to derive authalic camera latitude for unfold guide";
        return bundle;
    }
    const geo::Mat3 world_to_view = geo::multiply(
        geo::rotation_y(beta.value),
        geo::rotation_z(-radians(camera_longitude_deg))
    );
    const double radius_m = geo::authalic_radius_m();

    bundle.guides.reserve(24U);

    for (int longitude_deg = -150; longitude_deg <= 150; longitude_deg += 30) {
        if (!append_meridian(
                bundle.guides,
                static_cast<double>(longitude_deg),
                UnfoldGuideKind::graticule,
                target_mode,
                world_to_view,
                radius_m,
                canceled)) {
            if (should_cancel(canceled)) {
                bundle.canceled = true;
            } else {
                bundle.ok = false;
                bundle.diagnostic = "unable to build unfold meridian guide";
            }
            return bundle;
        }
    }

    for (int latitude_deg = -75; latitude_deg <= 75; latitude_deg += 15) {
        if (!append_parallel(
                bundle.guides,
                static_cast<double>(latitude_deg),
                target_mode,
                world_to_view,
                radius_m,
                canceled)) {
            if (should_cancel(canceled)) {
                bundle.canceled = true;
            } else {
                bundle.ok = false;
                bundle.diagnostic = "unable to build unfold parallel guide";
            }
            return bundle;
        }
    }

    if (!append_meridian(
            bundle.guides,
            -180.0,
            UnfoldGuideKind::seam,
            target_mode,
            world_to_view,
            radius_m,
            canceled) ||
        !append_meridian(
            bundle.guides,
            180.0,
            UnfoldGuideKind::seam,
            target_mode,
            world_to_view,
            radius_m,
            canceled)) {
        if (should_cancel(canceled)) {
            bundle.canceled = true;
        } else {
            bundle.ok = false;
            bundle.diagnostic = "unable to build unfold seam guides";
        }
        return bundle;
    }

    bundle.diagnostic = target_mode == ViewMode::sinusoidal
        ? "Verified Globe → Sinusoidal endpoints with non-normative geographic unfold guide"
        : "Verified Globe → Mollweide endpoints with non-normative geographic unfold guide";
    return bundle;
}

}  // namespace aeris::viewer
