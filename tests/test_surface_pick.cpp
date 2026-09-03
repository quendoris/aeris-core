// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/view/surface.hpp"

#include "aeris/geo/wgs84.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

int failures = 0;

void expect_true(const std::string_view name, const bool condition) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL " << name << '\n';
    }
}

[[nodiscard]] double angular_difference_deg(
    const double left,
    const double right
) noexcept {
    return std::abs(std::remainder(left - right, 360.0));
}

[[nodiscard]] const aeris::view::ProjectionSeamSample* stable_visible_sample(
    const aeris::view::ProjectionSeamGeometry& seam
) noexcept {
    for (std::size_t index = 2U; index + 2U < seam.samples.size(); ++index) {
        const auto& sample = seam.samples[index];
        if (sample.globe_visible && sample.globe_depth_normalized > 0.2) {
            return &sample;
        }
    }
    return nullptr;
}

void verify_pick_recovers_cut(
    const std::string_view name,
    const double camera_longitude_deg,
    const double camera_latitude_deg,
    const double expected_cut_deg
) {
    using aeris::view::SurfaceMode;

    const auto seam = aeris::view::build_projection_seam_geometry(
        SurfaceMode::sinu_mollweide,
        camera_longitude_deg,
        camera_latitude_deg,
        expected_cut_deg
    );
    expect_true(name, seam.ok);
    if (!seam.ok) {
        std::cerr << "  seam diagnostic=" << seam.diagnostic << '\n';
        return;
    }

    const auto* sample = stable_visible_sample(seam);
    expect_true(name, sample != nullptr);
    if (sample == nullptr) return;

    const auto picked = aeris::view::pick_projection_cut_from_globe(
        SurfaceMode::sinu_mollweide,
        camera_longitude_deg,
        camera_latitude_deg,
        sample->globe
    );
    expect_true(name, picked.ok);
    if (!picked.ok) {
        std::cerr << "  pick diagnostic=" << picked.diagnostic << '\n';
        return;
    }

    expect_true(
        name,
        angular_difference_deg(
            picked.projection_central_meridian_deg,
            expected_cut_deg
        ) <= 1e-7
    );
}

}  // namespace

int main() {
    using aeris::view::SurfaceMode;

    verify_pick_recovers_cut(
        "default-camera Globe pick recovers 37 degree cut",
        15.0,
        20.0,
        37.0
    );
    verify_pick_recovers_cut(
        "rotated-camera Globe pick recovers same 37 degree cut",
        -48.0,
        33.0,
        37.0
    );
    verify_pick_recovers_cut(
        "wrapped negative cut is recovered modulo 360 degrees",
        102.0,
        -24.0,
        -143.5
    );

    const double radius = aeris::geo::authalic_radius_m();
    const auto outside = aeris::view::pick_projection_cut_from_globe(
        SurfaceMode::sinu_mollweide,
        15.0,
        20.0,
        {2.0 * radius, 0.0}
    );
    expect_true("cut picker rejects points outside visible Globe disk", !outside.ok);

    const auto invalid_target = aeris::view::pick_projection_cut_from_globe(
        SurfaceMode::globe,
        15.0,
        20.0,
        {0.0, 0.0}
    );
    expect_true("cut picker rejects Globe as planar target", !invalid_target.ok);

    const auto invalid_camera = aeris::view::pick_projection_cut_from_globe(
        SurfaceMode::sinu_mollweide,
        15.0,
        91.0,
        {0.0, 0.0}
    );
    expect_true("cut picker rejects invalid camera latitude", !invalid_camera.ok);

    if (failures != 0) {
        std::cerr << failures << " surface pick assertion(s) failed\n";
        return EXIT_FAILURE;
    }

    std::cout << "surface_pick: PASS\n";
    return EXIT_SUCCESS;
}
