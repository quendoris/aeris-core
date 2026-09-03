// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/view/scene.hpp"
#include "aeris/view/unfold.hpp"

#include "aeris/geo/wgs84.hpp"
#include "aeris/geometry/geographic.hpp"
#include "aeris/projection/sinu_mollweide.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

[[nodiscard]] double radians(const double degrees) noexcept {
    return degrees * aeris::geo::kPi / 180.0;
}

[[nodiscard]] bool near(
    const double left,
    const double right,
    const double tolerance = 1e-9
) noexcept {
    return std::abs(left - right) <= tolerance;
}

[[nodiscard]] aeris::source::Feature make_feature(
    std::string stable_id,
    const double longitude_offset_deg
) {
    std::vector<aeris::geometry::GeodeticPoint> points{
        {radians(longitude_offset_deg - 4.0), radians(-4.0)},
        {radians(longitude_offset_deg + 4.0), radians(-4.0)},
        {radians(longitude_offset_deg + 4.0), radians(4.0)},
        {radians(longitude_offset_deg - 4.0), radians(4.0)},
    };
    auto canonical = aeris::geometry::canonicalize_wgs84_linear_ring(points);
    if (!canonical.ok()) return {};
    canonical.value.interior_side = aeris::geometry::RingInteriorSide::left;

    aeris::source::Feature feature{};
    feature.stable_id = std::move(stable_id);
    feature.source_id = feature.stable_id + ".source";
    aeris::source::FeatureRing ring{};
    ring.geometry = std::move(canonical.value);
    ring.role = aeris::source::RingRole::exterior;
    feature.rings.push_back(std::move(ring));
    return feature;
}

[[nodiscard]] aeris::geometry::GeodeticPoint world_from_philbrick_frame(
    const double frame_longitude_deg,
    const double frame_geodetic_latitude_deg
) {
    const auto beta = aeris::geo::authalic_latitude(
        radians(frame_geodetic_latitude_deg)
    );
    if (!beta.ok()) return {};

    const auto planar = aeris::projection::sinu_mollweide_forward(
        radians(frame_longitude_deg),
        beta.value
    );
    if (!planar.ok()) return {};

    const auto world = aeris::projection::philbrick_sinu_mollweide_inverse_wgs84(
        planar.value.x,
        planar.value.y
    );
    if (!world.ok()) return {};
    return {world.value.longitude_rad, world.value.latitude_rad};
}

[[nodiscard]] aeris::source::Feature make_philbrick_seam_feature() {
    std::vector<aeris::geometry::GeodeticPoint> points{
        world_from_philbrick_frame(179.5, -3.0),
        world_from_philbrick_frame(-179.5, -3.0),
        world_from_philbrick_frame(-179.5, 3.0),
        world_from_philbrick_frame(179.5, 3.0),
    };
    auto canonical = aeris::geometry::canonicalize_wgs84_linear_ring(points);
    if (!canonical.ok()) return {};
    canonical.value.interior_side = aeris::geometry::RingInteriorSide::left;

    aeris::source::Feature feature{};
    feature.stable_id = "feature.philbrick-seam";
    feature.source_id = "feature.philbrick-seam.source";
    aeris::source::FeatureRing ring{};
    ring.geometry = std::move(canonical.value);
    ring.role = aeris::source::RingRole::exterior;
    feature.rings.push_back(std::move(ring));
    return feature;
}

bool test_stable_identity_survives_scene_construction() {
    aeris::source::Result world{};
    world.features.push_back(make_feature("feature.alpha", -15.0));
    world.features.push_back(make_feature("feature.beta", 15.0));
    if (world.features[0].rings.empty() || world.features[1].rings.empty()) {
        std::cerr << "scene fixture failed to canonicalize\n";
        return false;
    }

    aeris::view::SceneRequest request{};
    request.mode = aeris::view::SurfaceMode::sinusoidal;
    request.quality = aeris::view::SceneQuality::verified;
    const auto scene = aeris::view::build_scene_geometry(world, request);
    if (!scene.ok || scene.canceled || scene.features.size() != 2U) {
        std::cerr << "verified flat scene failed: " << scene.diagnostic << '\n';
        return false;
    }
    if (scene.features[0].stable_id != "feature.alpha" ||
        scene.features[1].stable_id != "feature.beta" ||
        scene.features[0].fill_rings.empty() ||
        scene.features[1].fill_rings.empty()) {
        std::cerr << "scene lost canonical feature identity\n";
        return false;
    }
    return true;
}

bool test_sinu_mollweide_scene_uses_one_surface_with_movable_cut() {
    aeris::source::Result world{};
    world.features.push_back(make_philbrick_seam_feature());
    if (world.features.front().rings.empty()) {
        std::cerr << "Sinu-Mollweide scene fixture failed to canonicalize\n";
        return false;
    }

    aeris::view::SceneRequest request{};
    request.mode = aeris::view::SurfaceMode::sinu_mollweide;
    request.quality = aeris::view::SceneQuality::verified;
    request.projection_central_meridian_deg = 0.0;
    const auto cut_scene = aeris::view::build_scene_geometry(world, request);
    if (!cut_scene.ok || cut_scene.canceled || cut_scene.features.size() != 1U ||
        cut_scene.features.front().stable_id != "feature.philbrick-seam" ||
        cut_scene.features.front().fill_rings.size() != 2U ||
        cut_scene.features.front().outlines.size() != 2U ||
        !near(cut_scene.projection_central_meridian_deg, 0.0)) {
        std::cerr << "single Sinu-Mollweide cut scene failed: "
                  << cut_scene.diagnostic << '\n';
        return false;
    }

    request.projection_central_meridian_deg = 40.0;
    const auto moved_scene = aeris::view::build_scene_geometry(world, request);
    if (!moved_scene.ok || moved_scene.canceled || moved_scene.features.size() != 1U ||
        moved_scene.features.front().stable_id != "feature.philbrick-seam" ||
        moved_scene.features.front().fill_rings.size() != 1U ||
        moved_scene.features.front().outlines.size() != 1U ||
        !near(moved_scene.projection_central_meridian_deg, 40.0)) {
        std::cerr << "moved-cut Sinu-Mollweide scene failed: "
                  << moved_scene.diagnostic << '\n';
        return false;
    }

    if (aeris::view::surface_mode_name(aeris::view::SurfaceMode::sinu_mollweide) !=
        std::string("Sinu-Mollweide")) {
        std::cerr << "single Sinu-Mollweide surface has no stable public name\n";
        return false;
    }
    return true;
}

bool test_globe_preview_keeps_filled_composition() {
    aeris::source::Result world{};
    world.features.push_back(make_feature("feature.preview", 0.0));
    if (world.features.front().rings.empty()) return false;

    aeris::view::SceneRequest request{};
    request.mode = aeris::view::SurfaceMode::globe;
    request.quality = aeris::view::SceneQuality::preview;
    request.camera_longitude_deg = 0.0;
    request.camera_latitude_deg = 0.0;
    const auto scene = aeris::view::build_scene_geometry(world, request);

    if (!scene.ok || scene.canceled || scene.features.size() != 1U ||
        scene.features.front().fill_rings.empty() ||
        scene.features.front().outlines.empty() ||
        scene.fill_rings == 0U) {
        std::cerr << "globe preview lost filled composition: " << scene.diagnostic << '\n';
        return false;
    }
    return true;
}

bool test_unfold_contract_keeps_verified_endpoints_separate() {
    aeris::source::Result world{};
    world.features.push_back(make_feature("feature.unfold", 0.0));
    if (world.features.front().rings.empty()) return false;

    for (const auto target : {
             aeris::view::SurfaceMode::sinusoidal,
             aeris::view::SurfaceMode::mollweide,
         }) {
        const auto bundle = aeris::view::build_unfold_bundle(
            world,
            23.0,
            -11.0,
            target
        );
        if (!bundle.ok || bundle.canceled || bundle.guides.size() != 24U ||
            bundle.globe_endpoint.mode != aeris::view::SurfaceMode::globe ||
            bundle.flat_endpoint.mode != target ||
            bundle.globe_endpoint.quality != aeris::view::SceneQuality::verified ||
            bundle.flat_endpoint.quality != aeris::view::SceneQuality::verified ||
            !bundle.globe_endpoint.ok || !bundle.flat_endpoint.ok ||
            !near(bundle.globe_endpoint.camera_longitude_deg, 23.0) ||
            !near(bundle.globe_endpoint.camera_latitude_deg, -11.0)) {
            std::cerr << "unfold endpoint contract failed: " << bundle.diagnostic << '\n';
            return false;
        }

        const aeris::view::UnfoldGuideLine* seam_left = nullptr;
        const aeris::view::UnfoldGuideLine* seam_right = nullptr;
        for (const auto& guide : bundle.guides) {
            if (guide.kind != aeris::view::UnfoldGuideKind::seam) continue;
            if (seam_left == nullptr) seam_left = &guide;
            else seam_right = &guide;
        }
        if (seam_left == nullptr || seam_right == nullptr ||
            seam_left->vertices.size() != 37U ||
            seam_right->vertices.size() != 37U) {
            std::cerr << "unfold seam guides missing\n";
            return false;
        }

        constexpr std::size_t equator_index = 18U;
        const auto& left = seam_left->vertices[equator_index];
        const auto& right = seam_right->vertices[equator_index];
        if (!near(left.globe.x, right.globe.x, 1e-6) ||
            !near(left.globe.y, right.globe.y, 1e-6) ||
            !(left.flat.x < 0.0 && right.flat.x > 0.0) ||
            near(left.flat.x, right.flat.x, 1.0)) {
            std::cerr << "unfold seam does not open from one globe line into two flat edges\n";
            return false;
        }

        const auto at_start = aeris::view::interpolate_unfold_vertex(left, 0.0);
        const auto at_end = aeris::view::interpolate_unfold_vertex(left, 1.0);
        if (!near(at_start.x, left.globe.x) || !near(at_start.y, left.globe.y) ||
            !near(at_end.x, left.flat.x) || !near(at_end.y, left.flat.y)) {
            std::cerr << "unfold interpolation lost exact endpoints\n";
            return false;
        }
    }

    aeris::view::UnfoldGuideVertex behind{};
    behind.globe_depth_normalized = -0.5;
    aeris::view::UnfoldGuideVertex front{};
    front.globe_depth_normalized = 0.25;
    if (!near(aeris::view::unfold_guide_visibility(behind, 0.0), 0.0) ||
        !near(aeris::view::unfold_guide_visibility(behind, 1.0), 1.0) ||
        !near(aeris::view::unfold_guide_visibility(front, 0.0), 1.0) ||
        !near(aeris::view::unfold_guide_visibility(front, 0.5), 1.0)) {
        std::cerr << "unfold guide visibility contract failed\n";
        return false;
    }

    const auto invalid = aeris::view::build_unfold_bundle(
        world,
        0.0,
        0.0,
        aeris::view::SurfaceMode::globe
    );
    if (invalid.ok || invalid.canceled) {
        std::cerr << "unfold accepted Globe as a planar target\n";
        return false;
    }

    const auto canceled = aeris::view::build_unfold_bundle(
        world,
        0.0,
        0.0,
        aeris::view::SurfaceMode::sinusoidal,
        []() { return true; }
    );
    if (!canceled.canceled || !canceled.guides.empty()) {
        std::cerr << "unfold cancellation exposed completed transition geometry\n";
        return false;
    }
    return true;
}

bool test_invalid_source_preserves_primary_diagnostic() {
    aeris::source::Result world{};
    world.error = aeris::source::SourceError::normalization_failed;
    world.diagnostic = "primary source failure";

    aeris::view::SceneRequest request{};
    const auto scene = aeris::view::build_scene_geometry(world, request);
    if (scene.ok || scene.diagnostic != "primary source failure") {
        std::cerr << "scene rewrote invalid-source diagnostic\n";
        return false;
    }
    return true;
}

bool test_cancellation_never_publishes_partial_success() {
    aeris::source::Result world{};
    world.features.push_back(make_feature("feature.cancel", 0.0));
    if (world.features.front().rings.empty()) return false;

    aeris::view::SceneRequest request{};
    request.mode = aeris::view::SurfaceMode::globe;
    request.quality = aeris::view::SceneQuality::verified;
    const auto scene = aeris::view::build_scene_geometry(
        world,
        request,
        []() { return true; }
    );
    if (!scene.canceled || !scene.features.empty()) {
        std::cerr << "cancelled scene exposed partial completed features\n";
        return false;
    }
    return true;
}

}  // namespace

int main() {
    if (!test_stable_identity_survives_scene_construction() ||
        !test_sinu_mollweide_scene_uses_one_surface_with_movable_cut() ||
        !test_globe_preview_keeps_filled_composition() ||
        !test_unfold_contract_keeps_verified_endpoints_separate() ||
        !test_invalid_source_preserves_primary_diagnostic() ||
        !test_cancellation_never_publishes_partial_success()) {
        return EXIT_FAILURE;
    }
    std::cout << "scene_geometry: PASS\n";
    return EXIT_SUCCESS;
}
