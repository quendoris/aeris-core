// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/view/scene.hpp"

#include "aeris/geo/wgs84.hpp"
#include "aeris/geometry/geographic.hpp"

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

[[nodiscard]] aeris::source::Feature make_world_seam_feature() {
    std::vector<aeris::geometry::GeodeticPoint> points{
        {radians(179.5), radians(-3.0)},
        {radians(-179.5), radians(-3.0)},
        {radians(-179.5), radians(3.0)},
        {radians(179.5), radians(3.0)},
    };
    auto canonical = aeris::geometry::canonicalize_wgs84_linear_ring(points);
    if (!canonical.ok()) return {};
    canonical.value.interior_side = aeris::geometry::RingInteriorSide::left;

    aeris::source::Feature feature{};
    feature.stable_id = "feature.world-seam";
    feature.source_id = "feature.world-seam.source";
    aeris::source::FeatureRing ring{};
    ring.geometry = std::move(canonical.value);
    ring.role = aeris::source::RingRole::exterior;
    feature.rings.push_back(std::move(ring));
    return feature;
}

[[nodiscard]] bool verify_movable_cut(const aeris::view::SurfaceMode mode) {
    aeris::source::Result world{};
    world.features.push_back(make_world_seam_feature());
    if (world.features.front().rings.empty()) {
        std::cerr << aeris::view::surface_mode_name(mode)
                  << " movable-cut fixture failed to canonicalize\n";
        return false;
    }

    aeris::view::SceneRequest request{};
    request.mode = mode;
    request.quality = aeris::view::SceneQuality::verified;
    request.projection_central_meridian_deg = 0.0;

    const auto default_cut = aeris::view::build_scene_geometry(world, request);
    if (!default_cut.ok || default_cut.canceled ||
        default_cut.features.size() != 1U ||
        default_cut.features.front().stable_id != "feature.world-seam" ||
        default_cut.features.front().fill_rings.size() != 2U ||
        default_cut.features.front().outlines.size() != 2U ||
        !near(default_cut.projection_central_meridian_deg, 0.0)) {
        std::cerr << aeris::view::surface_mode_name(mode)
                  << " default-cut scene failed: " << default_cut.diagnostic << '\n';
        return false;
    }

    request.projection_central_meridian_deg = 40.0;
    const auto moved_cut = aeris::view::build_scene_geometry(world, request);
    if (!moved_cut.ok || moved_cut.canceled ||
        moved_cut.features.size() != 1U ||
        moved_cut.features.front().stable_id != "feature.world-seam" ||
        moved_cut.features.front().fill_rings.size() != 1U ||
        moved_cut.features.front().outlines.size() != 1U ||
        !near(moved_cut.projection_central_meridian_deg, 40.0)) {
        std::cerr << aeris::view::surface_mode_name(mode)
                  << " moved-cut scene failed: " << moved_cut.diagnostic << '\n';
        return false;
    }

    return true;
}

}  // namespace

int main() {
    if (!verify_movable_cut(aeris::view::SurfaceMode::sinusoidal) ||
        !verify_movable_cut(aeris::view::SurfaceMode::mollweide)) {
        return EXIT_FAILURE;
    }

    std::cout << "standard_projection_cut_scene: PASS\n";
    return EXIT_SUCCESS;
}
