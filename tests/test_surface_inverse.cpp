// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/geo/rotation.hpp"
#include "aeris/geo/wgs84.hpp"
#include "aeris/projection/sinu_mollweide.hpp"
#include "aeris/projection/wgs84.hpp"
#include "aeris/view/surface_inverse.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

[[nodiscard]] double radians(const double degrees) noexcept {
    return degrees * aeris::geo::kPi / 180.0;
}

[[nodiscard]] bool near(const double left, const double right, const double tolerance = 1e-8) {
    return std::abs(left - right) <= tolerance;
}

int fail(const int code, const std::string& diagnostic) {
    std::cerr << "aeris_test_surface_inverse: FAIL " << diagnostic << '\n';
    return code;
}

[[nodiscard]] aeris::geometry::PlanarPoint globe_forward(
    const double longitude_deg,
    const double latitude_deg,
    const double camera_longitude_deg,
    const double camera_latitude_deg
) {
    const auto beta = aeris::geo::authalic_latitude(radians(latitude_deg));
    const auto camera_beta = aeris::geo::authalic_latitude(radians(camera_latitude_deg));
    if (!beta.ok() || !camera_beta.ok()) return {};
    const aeris::geo::Vec3 world = aeris::geo::lonlat_to_unit_vector(
        radians(longitude_deg),
        beta.value
    );
    const aeris::geo::Mat3 world_to_view = aeris::geo::multiply(
        aeris::geo::rotation_y(camera_beta.value),
        aeris::geo::rotation_z(-radians(camera_longitude_deg))
    );
    const aeris::geo::Vec3 viewed = aeris::geo::apply(world_to_view, world);
    const double radius = aeris::geo::authalic_radius_m();
    // This is the exact orthographic_globe_point() public convention:
    // plane=(view.y, view.z), depth=view.x.
    return {viewed.y * radius, viewed.z * radius};
}

[[nodiscard]] aeris::projection::PlanarResult philbrick_forward_with_cut(
    const double longitude_deg,
    const double latitude_deg,
    const double cut_deg
) {
    const auto beta = aeris::geo::authalic_latitude(radians(latitude_deg));
    if (!beta.ok()) return {{}, beta.error};
    const aeris::geo::Vec3 world = aeris::geo::lonlat_to_unit_vector(
        radians(longitude_deg),
        beta.value
    );
    const aeris::geo::Vec3 framed = aeris::geo::apply(
        aeris::projection::philbrick_world_to_projection_matrix(),
        world
    );
    const auto framed_lonlat = aeris::geo::unit_vector_to_lonlat(framed);
    if (!framed_lonlat.ok()) return {{}, framed_lonlat.error};
    const auto pseudo_geodetic = aeris::geo::geodetic_latitude_from_authalic(
        framed_lonlat.value.latitude_rad
    );
    if (!pseudo_geodetic.ok()) return {{}, pseudo_geodetic.error};
    return aeris::projection::project_wgs84_primitive(
        framed_lonlat.value.longitude_rad,
        pseudo_geodetic.value,
        aeris::projection::EqualAreaPrimitive::sinu_mollweide,
        radians(cut_deg)
    );
}

}  // namespace

int main() {
    constexpr double longitude = 32.0;
    constexpr double latitude = 18.0;
    constexpr double camera_longitude = 32.0;
    constexpr double camera_latitude = 18.0;
    constexpr double cut = 37.0;

    const auto globe_point = globe_forward(
        longitude,
        latitude,
        camera_longitude,
        camera_latitude
    );
    const auto globe = aeris::view::pick_geographic_from_surface(
        aeris::view::SurfaceMode::globe,
        globe_point,
        camera_longitude,
        camera_latitude,
        cut
    );
    if (!globe.ok || !near(globe.longitude_deg, longitude) ||
        !near(globe.latitude_deg, latitude)) {
        return fail(1, "Globe inverse did not recover the source WGS84 point");
    }

    for (const auto mode : {
             aeris::view::SurfaceMode::sinusoidal,
             aeris::view::SurfaceMode::mollweide,
         }) {
        const auto primitive = mode == aeris::view::SurfaceMode::sinusoidal
            ? aeris::projection::EqualAreaPrimitive::sinusoidal
            : aeris::projection::EqualAreaPrimitive::mollweide;
        const auto projected = aeris::projection::project_wgs84_primitive(
            radians(longitude),
            radians(latitude),
            primitive,
            radians(cut)
        );
        if (!projected.ok()) return fail(2, "standard forward projection failed");
        const auto recovered = aeris::view::pick_geographic_from_surface(
            mode,
            projected.value,
            camera_longitude,
            camera_latitude,
            cut
        );
        if (!recovered.ok || !near(recovered.longitude_deg, longitude) ||
            !near(recovered.latitude_deg, latitude)) {
            return fail(3, "standard planar inverse did not round-trip WGS84");
        }
    }

    const auto philbrick_projected = philbrick_forward_with_cut(
        longitude,
        latitude,
        cut
    );
    if (!philbrick_projected.ok()) {
        return fail(4, "Philbrick forward projection failed");
    }
    const auto philbrick = aeris::view::pick_geographic_from_surface(
        aeris::view::SurfaceMode::sinu_mollweide,
        philbrick_projected.value,
        camera_longitude,
        camera_latitude,
        cut
    );
    if (!philbrick.ok || !near(philbrick.longitude_deg, longitude, 1e-7) ||
        !near(philbrick.latitude_deg, latitude, 1e-7)) {
        return fail(5, "Sinu-Mollweide inverse did not round-trip WGS84");
    }

    const double radius = aeris::geo::authalic_radius_m();
    const auto outside = aeris::view::pick_geographic_from_surface(
        aeris::view::SurfaceMode::globe,
        {radius * 1.01, 0.0},
        camera_longitude,
        camera_latitude,
        cut
    );
    if (outside.ok) {
        return fail(6, "Globe inverse accepted a point outside the visible disk");
    }

    std::cout
        << "aeris_test_surface_inverse: PASS"
        << " lon=" << philbrick.longitude_deg
        << " lat=" << philbrick.latitude_deg
        << '\n';
    return EXIT_SUCCESS;
}
