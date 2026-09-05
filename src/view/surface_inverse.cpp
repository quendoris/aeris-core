// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/view/surface_inverse.hpp"

#include "aeris/geo/rotation.hpp"
#include "aeris/geo/wgs84.hpp"
#include "aeris/projection/primitives.hpp"
#include "aeris/projection/sinu_mollweide.hpp"

#include <algorithm>
#include <cmath>

namespace aeris::view {
namespace {

[[nodiscard]] double radians(const double degrees) noexcept {
    return degrees * geo::kPi / 180.0;
}

[[nodiscard]] double degrees(const double radians_value) noexcept {
    return radians_value * 180.0 / geo::kPi;
}

[[nodiscard]] double wrap_longitude_deg(double value) noexcept {
    value = std::fmod(value + 180.0, 360.0);
    if (value < 0.0) value += 360.0;
    return value - 180.0;
}

[[nodiscard]] SurfaceGeographicPickResult failure(const char* diagnostic) noexcept {
    SurfaceGeographicPickResult result{};
    result.diagnostic = diagnostic;
    return result;
}

[[nodiscard]] SurfaceGeographicPickResult from_authalic_world_vector(
    const geo::Vec3 world,
    const double fallback_longitude_deg
) noexcept {
    const geo::LonLatResult authalic = geo::unit_vector_to_lonlat(world);
    if (!authalic.ok() && authalic.error != geo::MathError::indeterminate_coordinate) {
        return failure("surface inverse could not recover authalic world coordinates");
    }

    const geo::ScalarResult geodetic =
        geo::geodetic_latitude_from_authalic(authalic.value.latitude_rad);
    if (!geodetic.ok()) {
        return failure("surface inverse could not recover WGS84 latitude");
    }

    SurfaceGeographicPickResult result{};
    result.longitude_indeterminate =
        authalic.error == geo::MathError::indeterminate_coordinate;
    result.longitude_deg = result.longitude_indeterminate
        ? wrap_longitude_deg(fallback_longitude_deg)
        : wrap_longitude_deg(degrees(authalic.value.longitude_rad));
    result.latitude_deg = degrees(geodetic.value);
    result.ok = true;
    return result;
}

[[nodiscard]] SurfaceGeographicPickResult inverse_globe(
    const geometry::PlanarPoint point,
    const double camera_longitude_deg,
    const double camera_latitude_deg
) noexcept {
    if (!std::isfinite(point.x) || !std::isfinite(point.y) ||
        !std::isfinite(camera_longitude_deg) || !std::isfinite(camera_latitude_deg) ||
        camera_latitude_deg < -90.0 || camera_latitude_deg > 90.0) {
        return failure("Globe inverse received non-finite or invalid camera input");
    }

    const double radius = geo::authalic_radius_m();
    const double x = point.x / radius;
    const double y = point.y / radius;
    const double radial2 = x * x + y * y;
    constexpr double tolerance = 1e-12;
    if (radial2 > 1.0 + tolerance) {
        return failure("surface point lies outside the visible Globe disk");
    }
    const double z = std::sqrt(std::max(0.0, 1.0 - radial2));

    const geo::ScalarResult camera_beta =
        geo::authalic_latitude(radians(camera_latitude_deg));
    if (!camera_beta.ok()) {
        return failure("Globe inverse could not derive authalic camera latitude");
    }
    const geo::Mat3 world_to_view = geo::multiply(
        geo::rotation_y(camera_beta.value),
        geo::rotation_z(-radians(camera_longitude_deg))
    );
    const geo::Vec3 world = geo::apply(
        geo::transpose(world_to_view),
        {x, y, z}
    );
    return from_authalic_world_vector(world, camera_longitude_deg);
}

[[nodiscard]] SurfaceGeographicPickResult inverse_standard_flat(
    const SurfaceMode mode,
    const geometry::PlanarPoint point,
    const double central_meridian_deg
) noexcept {
    projection::SphericalResult inverse{};
    if (mode == SurfaceMode::sinusoidal) {
        inverse = projection::sinusoidal_inverse(point.x, point.y);
    } else {
        inverse = projection::mollweide_inverse(point.x, point.y);
    }
    if (!inverse.ok() && inverse.error != geo::MathError::indeterminate_coordinate) {
        return failure("planar surface point is outside the selected projection");
    }

    const geo::ScalarResult geodetic =
        geo::geodetic_latitude_from_authalic(inverse.value.latitude_rad);
    if (!geodetic.ok()) {
        return failure("planar inverse could not recover WGS84 latitude");
    }

    SurfaceGeographicPickResult result{};
    result.longitude_indeterminate =
        inverse.error == geo::MathError::indeterminate_coordinate;
    result.longitude_deg = result.longitude_indeterminate
        ? wrap_longitude_deg(central_meridian_deg)
        : wrap_longitude_deg(
            degrees(inverse.value.longitude_rad) + central_meridian_deg
        );
    result.latitude_deg = degrees(geodetic.value);
    result.ok = true;
    return result;
}

[[nodiscard]] SurfaceGeographicPickResult inverse_sinu_mollweide(
    const geometry::PlanarPoint point,
    const double central_meridian_deg
) noexcept {
    const projection::SphericalResult inverse =
        projection::sinu_mollweide_inverse(point.x, point.y);
    if (!inverse.ok() && inverse.error != geo::MathError::indeterminate_coordinate) {
        return failure("Sinu-Mollweide point is outside the projection surface");
    }

    const bool framed_longitude_indeterminate =
        inverse.error == geo::MathError::indeterminate_coordinate;
    const double framed_longitude_rad = framed_longitude_indeterminate
        ? radians(central_meridian_deg)
        : inverse.value.longitude_rad + radians(central_meridian_deg);

    const geo::Vec3 framed = geo::lonlat_to_unit_vector(
        framed_longitude_rad,
        inverse.value.latitude_rad
    );
    const geo::Vec3 world = geo::apply(
        geo::transpose(projection::philbrick_world_to_projection_matrix()),
        framed
    );
    SurfaceGeographicPickResult result =
        from_authalic_world_vector(world, central_meridian_deg);
    if (result.ok && framed_longitude_indeterminate) {
        // At a projection-frame pole there is no unique sheet longitude even
        // though rotating the vector back can yield a finite world longitude.
        // Preserve that semantic ambiguity for cursor/focus callers.
        result.longitude_indeterminate = true;
    }
    return result;
}

}  // namespace

SurfaceGeographicPickResult pick_geographic_from_surface(
    const SurfaceMode mode,
    const geometry::PlanarPoint surface_point,
    const double camera_longitude_deg,
    const double camera_latitude_deg,
    const double projection_central_meridian_deg
) noexcept {
    if (!std::isfinite(projection_central_meridian_deg)) {
        return failure("surface inverse received a non-finite projection cut");
    }

    switch (mode) {
    case SurfaceMode::globe:
        return inverse_globe(
            surface_point,
            camera_longitude_deg,
            camera_latitude_deg
        );
    case SurfaceMode::sinusoidal:
    case SurfaceMode::mollweide:
        return inverse_standard_flat(
            mode,
            surface_point,
            projection_central_meridian_deg
        );
    case SurfaceMode::sinu_mollweide:
        return inverse_sinu_mollweide(
            surface_point,
            projection_central_meridian_deg
        );
    }
    return failure("surface inverse received an unknown surface mode");
}

}  // namespace aeris::view
