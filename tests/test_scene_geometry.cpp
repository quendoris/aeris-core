// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/view/scene.hpp"

#include "aeris/geo/wgs84.hpp"
#include "aeris/geometry/geographic.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

[[nodiscard]] double radians(const double degrees) noexcept {
    return degrees * aeris::geo::kPi / 180.0;
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
        !test_invalid_source_preserves_primary_diagnostic() ||
        !test_cancellation_never_publishes_partial_success()) {
        return EXIT_FAILURE;
    }
    std::cout << "scene_geometry: PASS\n";
    return EXIT_SUCCESS;
}
