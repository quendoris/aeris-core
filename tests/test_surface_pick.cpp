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
    const aeris::view::ProjectionSeamSample* best = nullptr;
    for (std::size_t index = 2U; index + 2U < seam.samples.size(); ++index) {
        const auto& sample = seam.samples[index];
        if (!sample.globe_visible) continue;
        if (best == nullptr ||
            sample.globe_depth_normalized > best->globe_depth_normalized) {
            best = &sample;
        }
    }
    return best;
}

void verify_pick_recovers_cut(
    const std::string_view name,
    const aeris::view::SurfaceMode mode,
    const double camera_longitude_deg,
    const double camera_latitude_deg,
    const double expected_cut_deg
) {
    const auto seam = aeris::view::build_projection_seam_geometry(
        mode,
        camera_longitude_deg,
        camera_latitude_deg,
        expected_cut_deg
    );
    expect_true(name, seam.ok);
    if (!seam.ok) {
        std::cerr << "  seam diagnostic=" << seam.diagnostic << '\n';
        return;
    }

    // A cut can lie almost entirely on the far hemisphere for some cameras.
    // Select its deepest visible non-pole sample rather than requiring an
    // arbitrary minimum depth; this is both discoverable in the UI and the
    // best-conditioned point for recovering the cut longitude.
    const auto* sample = stable_visible_sample(seam);
    expect_true(name, sample != nullptr);
    if (sample == nullptr) return;

    const auto picked = aeris::view::pick_projection_cut_from_globe(
        mode,
        camera_longitude_deg,
        camera_latitude_deg,
        sample->globe
    );
    expect_true(name, picked.ok);
    if (!picked.ok) {
        std::cerr << "  pick diagnostic=" << picked.diagnostic << '\n';
        return;
    }

    const double difference = angular_difference_deg(
        picked.projection_central_meridian_deg,
        expected_cut_deg
    );
    if (difference > 1e-7) {
        std::cerr
            << "  expected cut=" << expected_cut_deg
            << " picked=" << picked.projection_central_meridian_deg
            << " difference=" << difference << " deg\n";
    }
    expect_true(name, difference <= 1e-7);
}

}  // namespace

int main() {
    using aeris::view::SurfaceMode;

    for (const SurfaceMode mode : {
             SurfaceMode::sinusoidal,
             SurfaceMode::mollweide,
             SurfaceMode::sinu_mollweide,
         }) {
        const char* mode_name = aeris::view::surface_mode_name(mode);
        const std::string default_name =
            std::string(mode_name) + " default-camera pick recovers 37 degree cut";
        verify_pick_recovers_cut(
            default_name,
            mode,
            15.0,
            20.0,
            37.0
        );

        const std::string rotated_name =
            std::string(mode_name) + " rotated-camera pick recovers 37 degree cut";
        verify_pick_recovers_cut(
            rotated_name,
            mode,
            -48.0,
            33.0,
            37.0
        );

        const std::string wrapped_name =
            std::string(mode_name) + " wrapped negative cut recovers modulo 360";
        verify_pick_recovers_cut(
            wrapped_name,
            mode,
            102.0,
            -24.0,
            -143.5
        );
    }

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
