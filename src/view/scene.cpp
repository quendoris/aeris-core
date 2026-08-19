// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/view/scene.hpp"

#include "aeris/geo/rotation.hpp"
#include "aeris/geo/wgs84.hpp"
#include "aeris/projection/ring.hpp"
#include "aeris/view/globe_curve.hpp"
#include "aeris/view/globe_polygon.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

namespace aeris::view {
namespace {

[[nodiscard]] double radians(const double degrees) noexcept {
    return degrees * geo::kPi / 180.0;
}

[[nodiscard]] bool should_cancel(const CancelCheck& canceled) {
    return static_cast<bool>(canceled) && canceled();
}

void include_point(SceneGeometry& scene, const geometry::PlanarPoint point) {
    if (!std::isfinite(point.x) || !std::isfinite(point.y)) return;
    scene.min_x = std::min(scene.min_x, point.x);
    scene.min_y = std::min(scene.min_y, point.y);
    scene.max_x = std::max(scene.max_x, point.x);
    scene.max_y = std::max(scene.max_y, point.y);
}

void initialize_flat_bounds(SceneGeometry& scene) {
    const double infinity = std::numeric_limits<double>::infinity();
    scene.min_x = infinity;
    scene.min_y = infinity;
    scene.max_x = -infinity;
    scene.max_y = -infinity;
}

void finalize_flat_bounds(SceneGeometry& scene) {
    if (!std::isfinite(scene.min_x) || !std::isfinite(scene.min_y) ||
        !std::isfinite(scene.max_x) || !std::isfinite(scene.max_y) ||
        scene.max_x <= scene.min_x || scene.max_y <= scene.min_y) {
        scene.ok = false;
        scene.diagnostic = "flat scene produced invalid bounds";
    }
}

[[nodiscard]] SceneGeometry build_globe_preview(
    const source::Result& world,
    const SceneRequest& request,
    const CancelCheck& canceled
) {
    SceneGeometry scene{};
    scene.mode = SurfaceMode::globe;
    scene.quality = SceneQuality::preview;
    scene.camera_longitude_deg = request.camera_longitude_deg;
    scene.camera_latitude_deg = request.camera_latitude_deg;
    scene.globe_radius_m = geo::authalic_radius_m();
    scene.min_x = -scene.globe_radius_m;
    scene.min_y = -scene.globe_radius_m;
    scene.max_x = scene.globe_radius_m;
    scene.max_y = scene.globe_radius_m;

    const auto beta = geo::authalic_latitude(radians(request.camera_latitude_deg));
    if (!beta.ok()) {
        scene.ok = false;
        scene.diagnostic = "unable to derive authalic camera latitude";
        return scene;
    }
    const geo::Mat3 world_to_view = geo::multiply(
        geo::rotation_y(beta.value),
        geo::rotation_z(-radians(request.camera_longitude_deg))
    );

    GlobeCurveOptions options{};
    options.geometric_tolerance_m = 25'000.0;
    options.horizon_tolerance_m = 0.25;
    options.max_subdivision_depth = 24U;
    options.max_root_iterations = 64U;
    options.max_segments = 250'000U;

    scene.features.reserve(world.features.size());
    for (const auto& feature : world.features) {
        if (should_cancel(canceled)) {
            scene.canceled = true;
            return scene;
        }

        SceneFeatureGeometry output{};
        output.stable_id = feature.stable_id;
        for (const auto& source_ring : feature.rings) {
            const auto curve = project_visible_wgs84_linear_ring(
                source_ring.geometry,
                world_to_view,
                options,
                scene.globe_radius_m
            );
            if (!curve.ok()) {
                scene.ok = false;
                scene.diagnostic = "globe preview curve failed for " + feature.stable_id;
                return scene;
            }
            for (const auto& part : curve.visible_parts) {
                if (part.size() >= 2U) {
                    output.outlines.push_back(part);
                    ++scene.outline_parts;
                    scene.vertices += part.size();
                }
            }
        }
        scene.features.push_back(std::move(output));
    }

    scene.diagnostic = "interactive wireframe preview";
    return scene;
}

[[nodiscard]] SceneGeometry build_globe_verified(
    const source::Result& world,
    const SceneRequest& request,
    const CancelCheck& canceled
) {
    SceneGeometry scene{};
    scene.mode = SurfaceMode::globe;
    scene.quality = SceneQuality::verified;
    scene.camera_longitude_deg = request.camera_longitude_deg;
    scene.camera_latitude_deg = request.camera_latitude_deg;
    scene.globe_radius_m = geo::authalic_radius_m();
    scene.min_x = -scene.globe_radius_m;
    scene.min_y = -scene.globe_radius_m;
    scene.max_x = scene.globe_radius_m;
    scene.max_y = scene.globe_radius_m;

    const auto beta = geo::authalic_latitude(radians(request.camera_latitude_deg));
    if (!beta.ok()) {
        scene.ok = false;
        scene.diagnostic = "unable to derive authalic camera latitude";
        return scene;
    }
    const geo::Mat3 world_to_view = geo::multiply(
        geo::rotation_y(beta.value),
        geo::rotation_z(-radians(request.camera_longitude_deg))
    );

    VerifiedGlobePolygonOptions verified_options{};
    verified_options.initial.curve.geometric_tolerance_m = 5'000.0;
    verified_options.initial.curve.horizon_tolerance_m = 0.01;
    verified_options.initial.curve.max_subdivision_depth = 32U;
    verified_options.initial.curve.max_root_iterations = 80U;
    verified_options.initial.curve.max_segments = 1'000'000U;
    verified_options.initial.horizon_arc_tolerance_m = 500.0;
    verified_options.initial.max_horizon_arc_segments = 1'000'000U;
    verified_options.initial.max_output_rings = 4096U;
    verified_options.relative_area_stability_tolerance = 5e-3;
    verified_options.absolute_area_stability_tolerance_m2 = 1.0;
    verified_options.max_refinement_rounds = 18U;

    scene.features.reserve(world.features.size());
    for (const auto& feature : world.features) {
        if (should_cancel(canceled)) {
            scene.canceled = true;
            return scene;
        }

        SceneFeatureGeometry output{};
        output.stable_id = feature.stable_id;
        for (const auto& source_ring : feature.rings) {
            if (should_cancel(canceled)) {
                scene.canceled = true;
                return scene;
            }

            const auto verified = project_visible_wgs84_linear_polygon_ring_verified(
                source_ring.geometry,
                world_to_view,
                verified_options,
                scene.globe_radius_m
            );
            if (!verified.ok()) {
                scene.ok = false;
                scene.diagnostic = "verified globe fill failed for " + feature.stable_id +
                    " (verification error " +
                    std::to_string(static_cast<unsigned>(verified.error)) + ")";
                return scene;
            }

            scene.max_refinement_rounds = std::max(
                scene.max_refinement_rounds,
                verified.refinement_rounds
            );
            for (const auto& ring : verified.polygon.rings) {
                if (ring.size() >= 3U) {
                    output.fill_rings.push_back(ring);
                    ++scene.fill_rings;
                    scene.vertices += ring.size();
                }
            }

            GlobeCurveOptions curve_options = verified_options.initial.curve;
            curve_options.geometric_tolerance_m = verified.final_curve_geometric_tolerance_m;
            const auto coastline = project_visible_wgs84_linear_ring(
                source_ring.geometry,
                world_to_view,
                curve_options,
                scene.globe_radius_m
            );
            if (!coastline.ok() ||
                coastline.horizon_crossings != verified.polygon.horizon_crossings) {
                scene.ok = false;
                scene.diagnostic = "independent verified coastline disagreed with fill for " +
                    feature.stable_id;
                return scene;
            }
            for (const auto& part : coastline.visible_parts) {
                if (part.size() >= 2U) {
                    output.outlines.push_back(part);
                    ++scene.outline_parts;
                    scene.vertices += part.size();
                }
            }
        }
        scene.features.push_back(std::move(output));
    }

    scene.diagnostic = "verified horizon topology and filled visible regions";
    return scene;
}

[[nodiscard]] SceneGeometry build_flat_verified(
    const source::Result& world,
    const SceneRequest& request,
    const CancelCheck& canceled
) {
    SceneGeometry scene{};
    scene.mode = request.mode;
    scene.quality = SceneQuality::verified;
    scene.camera_longitude_deg = request.camera_longitude_deg;
    scene.camera_latitude_deg = request.camera_latitude_deg;
    initialize_flat_bounds(scene);

    projection::RingProjectionOptions options{};
    options.primitive = request.mode == SurfaceMode::sinusoidal
        ? projection::EqualAreaPrimitive::sinusoidal
        : projection::EqualAreaPrimitive::mollweide;
    options.central_meridian_rad = 0.0;
    options.relative_area_tolerance = 1e-7;
    options.absolute_area_tolerance_m2 = 10'000.0;
    options.initial_geometric_tolerance_m = 2'000.0;
    options.initial_local_area_tolerance_m2 = 1.0e8;
    options.max_refinement_rounds = 18U;
    options.subdivision_max_depth = 32U;
    options.subdivision_max_segments_per_edge = 1'000'000U;
    options.max_projection_pieces = 4096U;

    scene.features.reserve(world.features.size());
    for (const auto& feature : world.features) {
        if (should_cancel(canceled)) {
            scene.canceled = true;
            return scene;
        }

        SceneFeatureGeometry output{};
        output.stable_id = feature.stable_id;
        for (const auto& source_ring : feature.rings) {
            if (should_cancel(canceled)) {
                scene.canceled = true;
                return scene;
            }

            const auto projected = projection::project_wgs84_linear_ring_piecewise_verified(
                source_ring.geometry,
                options
            );
            if (!projected.ok()) {
                scene.ok = false;
                scene.diagnostic = "verified flat projection failed for " + feature.stable_id +
                    " (error " +
                    std::to_string(static_cast<unsigned>(projected.error)) + ")";
                return scene;
            }

            scene.max_refinement_rounds = std::max(
                scene.max_refinement_rounds,
                projected.max_piece_refinement_rounds
            );
            for (const auto& piece : projected.pieces) {
                if (piece.size() < 3U) continue;
                output.fill_rings.push_back(piece);
                output.outlines.push_back(piece);
                ++scene.fill_rings;
                ++scene.outline_parts;
                scene.vertices += piece.size() * 2U;
                for (const auto point : piece) include_point(scene, point);
            }
        }
        scene.features.push_back(std::move(output));
    }

    finalize_flat_bounds(scene);
    if (scene.ok) {
        scene.diagnostic = request.mode == SurfaceMode::sinusoidal
            ? "verified WGS84 -> authalic -> Sinusoidal"
            : "verified WGS84 -> authalic -> Mollweide";
    }
    return scene;
}

}  // namespace

const char* surface_mode_name(const SurfaceMode mode) noexcept {
    switch (mode) {
    case SurfaceMode::globe:
        return "Globe";
    case SurfaceMode::sinusoidal:
        return "Sinusoidal";
    case SurfaceMode::mollweide:
        return "Mollweide";
    }
    return "Unknown";
}

SceneGeometry build_scene_geometry(
    const source::Result& world,
    const SceneRequest& request,
    const CancelCheck& canceled
) {
    if (!world.ok()) {
        SceneGeometry scene{};
        scene.mode = request.mode;
        scene.quality = request.quality;
        scene.ok = false;
        scene.diagnostic = world.diagnostic.empty()
            ? "source result is not valid"
            : world.diagnostic;
        return scene;
    }
    if (request.mode == SurfaceMode::globe) {
        return request.quality == SceneQuality::preview
            ? build_globe_preview(world, request, canceled)
            : build_globe_verified(world, request, canceled);
    }
    return build_flat_verified(world, request, canceled);
}

}  // namespace aeris::view
