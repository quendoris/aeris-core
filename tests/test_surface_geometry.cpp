// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/view/surface.hpp"

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

    if (failures != 0) {
        std::cerr << failures << " surface geometry assertion(s) failed\n";
        return EXIT_FAILURE;
    }

    std::cout << "surface_geometry: PASS\n";
    return EXIT_SUCCESS;
}
