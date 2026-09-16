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

[[nodiscard]] bool nearly_equal(
    const double left,
    const double right,
    const double tolerance
) noexcept {
    return std::abs(left - right) <= tolerance;
}

[[nodiscard]] double signed_area2(const aeris::view::PlanarSurfaceGeometry& surface) {
    long double sum = 0.0L;
    for (std::size_t index = 0U; index < surface.outline.size(); ++index) {
        const auto& a = surface.outline[index];
        const auto& b = surface.outline[(index + 1U) % surface.outline.size()];
        sum += static_cast<long double>(a.x) * static_cast<long double>(b.y) -
            static_cast<long double>(b.x) * static_cast<long double>(a.y);
    }
    return static_cast<double>(sum);
}

void verify_planar_surface(
    const std::string_view name,
    const aeris::view::SurfaceMode mode,
    const std::size_t minimum_vertices
) {
    const auto surface = aeris::view::build_planar_surface_geometry(mode);
    expect_true(name, surface.ok);
    if (!surface.ok) {
        std::cerr << "  diagnostic=" << surface.diagnostic << '\n';
        return;
    }

    expect_true(name, surface.mode == mode);
    expect_true(name, surface.outline.size() >= minimum_vertices);
    expect_true(name, std::isfinite(surface.min_x));
    expect_true(name, std::isfinite(surface.min_y));
    expect_true(name, std::isfinite(surface.max_x));
    expect_true(name, std::isfinite(surface.max_y));
    expect_true(name, surface.max_x > surface.min_x);
    expect_true(name, surface.max_y > surface.min_y);
    expect_true(name, signed_area2(surface) > 0.0);

    for (const auto point : surface.outline) {
        expect_true(name, std::isfinite(point.x) && std::isfinite(point.y));
        expect_true(
            name,
            point.x >= surface.min_x && point.x <= surface.max_x &&
            point.y >= surface.min_y && point.y <= surface.max_y
        );
    }
}

void verify_projection_seam() {
    using aeris::view::SurfaceMode;

    const auto base = aeris::view::build_projection_seam_geometry(
        SurfaceMode::sinu_mollweide,
        15.0,
        20.0,
        0.0
    );
    const auto moved_cut = aeris::view::build_projection_seam_geometry(
        SurfaceMode::sinu_mollweide,
        15.0,
        20.0,
        67.0
    );
    const auto moved_camera = aeris::view::build_projection_seam_geometry(
        SurfaceMode::sinu_mollweide,
        -42.0,
        31.0,
        0.0
    );

    expect_true("Sinu-Mollweide seam builds", base.ok);
    expect_true("moved Sinu-Mollweide cut builds", moved_cut.ok);
    expect_true("moved Globe camera seam builds", moved_camera.ok);
    if (!base.ok || !moved_cut.ok || !moved_camera.ok) return;

    expect_true("seam keeps exact target mode", base.mode == SurfaceMode::sinu_mollweide);
    expect_true("seam samples full pole-to-pole cut", base.samples.size() >= 181U);
    expect_true("moved cut preserves seam sample count", moved_cut.samples.size() == base.samples.size());
    expect_true("moved camera preserves seam sample count", moved_camera.samples.size() == base.samples.size());

    const double radius = aeris::geo::authalic_radius_m();
    const double radius2 = radius * radius;
    bool saw_visible = false;
    bool saw_hidden = false;
    bool cut_changed_globe = false;
    bool camera_changed_globe = false;

    for (std::size_t index = 0U; index < base.samples.size(); ++index) {
        const auto& sample = base.samples[index];
        const auto& cut_sample = moved_cut.samples[index];
        const auto& camera_sample = moved_camera.samples[index];

        expect_true(
            "seam Globe sample is finite",
            std::isfinite(sample.globe.x) &&
            std::isfinite(sample.globe.y) &&
            std::isfinite(sample.globe_depth_normalized)
        );
        expect_true(
            "seam planar samples are finite",
            std::isfinite(sample.flat_left.x) &&
            std::isfinite(sample.flat_left.y) &&
            std::isfinite(sample.flat_right.x) &&
            std::isfinite(sample.flat_right.y)
        );
        expect_true(
            "left and right seam sides share northing",
            nearly_equal(sample.flat_left.y, sample.flat_right.y, 1e-7)
        );
        expect_true(
            "left seam side does not cross right side",
            sample.flat_left.x <= sample.flat_right.x + 1e-7
        );
        expect_true(
            "seam visibility agrees with Globe depth",
            sample.globe_visible == (sample.globe_depth_normalized >= 0.0)
        );

        const double reconstructed_radius2 =
            sample.globe.x * sample.globe.x +
            sample.globe.y * sample.globe.y +
            radius2 * sample.globe_depth_normalized * sample.globe_depth_normalized;
        expect_true(
            "folded seam sample remains on authalic sphere",
            std::abs(reconstructed_radius2 - radius2) <= radius2 * 1e-10
        );

        // Moving the cut or the camera must never deform the flat sheet. Only
        // the folded Globe location changes.
        expect_true(
            "moving cut preserves left flat boundary",
            nearly_equal(sample.flat_left.x, cut_sample.flat_left.x, 1e-7) &&
            nearly_equal(sample.flat_left.y, cut_sample.flat_left.y, 1e-7)
        );
        expect_true(
            "moving cut preserves right flat boundary",
            nearly_equal(sample.flat_right.x, cut_sample.flat_right.x, 1e-7) &&
            nearly_equal(sample.flat_right.y, cut_sample.flat_right.y, 1e-7)
        );
        expect_true(
            "moving camera preserves left flat boundary",
            nearly_equal(sample.flat_left.x, camera_sample.flat_left.x, 1e-7) &&
            nearly_equal(sample.flat_left.y, camera_sample.flat_left.y, 1e-7)
        );
        expect_true(
            "moving camera preserves right flat boundary",
            nearly_equal(sample.flat_right.x, camera_sample.flat_right.x, 1e-7) &&
            nearly_equal(sample.flat_right.y, camera_sample.flat_right.y, 1e-7)
        );

        if (std::hypot(
                sample.globe.x - cut_sample.globe.x,
                sample.globe.y - cut_sample.globe.y
            ) > 1.0) {
            cut_changed_globe = true;
        }
        if (std::hypot(
                sample.globe.x - camera_sample.globe.x,
                sample.globe.y - camera_sample.globe.y
            ) > 1.0) {
            camera_changed_globe = true;
        }

        saw_visible = saw_visible || sample.globe_visible;
        saw_hidden = saw_hidden || !sample.globe_visible;
    }

    expect_true("default seam has a visible Globe arc", saw_visible);
    expect_true("default seam also passes behind Globe", saw_hidden);
    expect_true("moving projection cut moves physical Globe seam", cut_changed_globe);
    expect_true("moving Globe camera moves seam presentation", camera_changed_globe);

    const auto ordinary = aeris::view::build_projection_seam_geometry(
        SurfaceMode::sinusoidal,
        15.0,
        20.0,
        30.0
    );
    expect_true("diagnostic Sinusoidal seam uses shared contract", ordinary.ok);

    const auto invalid_globe = aeris::view::build_projection_seam_geometry(
        SurfaceMode::globe,
        15.0,
        20.0,
        0.0
    );
    expect_true("Globe alone rejects projection seam target", !invalid_globe.ok);

    const auto invalid_camera = aeris::view::build_projection_seam_geometry(
        SurfaceMode::sinu_mollweide,
        15.0,
        91.0,
        0.0
    );
    expect_true("projection seam rejects invalid camera latitude", !invalid_camera.ok);
}

}  // namespace

int main() {
    verify_planar_surface(
        "Sinusoidal surface envelope",
        aeris::view::SurfaceMode::sinusoidal,
        300U
    );
    verify_planar_surface(
        "Mollweide surface envelope",
        aeris::view::SurfaceMode::mollweide,
        300U
    );
    verify_planar_surface(
        "Sinu-Mollweide surface envelope",
        aeris::view::SurfaceMode::sinu_mollweide,
        300U
    );

    const auto globe = aeris::view::build_planar_surface_geometry(
        aeris::view::SurfaceMode::globe
    );
    expect_true("Globe rejects planar envelope request", !globe.ok);
    expect_true("Globe planar envelope is empty", globe.outline.empty());

    verify_projection_seam();

    if (failures != 0) {
        std::cerr << failures << " surface geometry assertion(s) failed\n";
        return EXIT_FAILURE;
    }

    std::cout << "surface_geometry: PASS\n";
    return EXIT_SUCCESS;
}
