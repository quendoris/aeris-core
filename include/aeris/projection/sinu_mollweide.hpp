// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "aeris/geo/rotation.hpp"
#include "aeris/projection/primitives.hpp"

namespace aeris::projection {

// Latitude where the Sinusoidal and Mollweide horizontal scales coincide for
// the spherical primitives used by AERIS. It is approximately 40°44′12″.
inline constexpr double kSinuMollweideTransitionLatitudeRad =
    0.71098888148384444440;

// Normalized northing needed to make the asymmetric Philbrick fusion
// continuous at -kSinuMollweideTransitionLatitudeRad.
inline constexpr double kSinuMollweideNorthingOffsetRatio =
    0.05280352736854078837;

inline constexpr double kPhilbrickCenterLongitudeRad =
    20.0 * geo::kPi / 180.0;
inline constexpr double kPhilbrickCenterGeodeticLatitudeRad =
    55.0 * geo::kPi / 180.0;

// Allen K. Philbrick's default oblique aspect as an orientation-preserving
// rotation on the authalic sphere. Keeping the frame explicit is important:
// the planar seam belongs to this rotated frame rather than to an ordinary
// WGS84 meridian. Frontends may later compose an additional rotation when a
// user deliberately moves the cut before unfolding, while this function
// remains the reproducible Wide-Angle default.
[[nodiscard]] geo::Mat3 philbrick_world_to_projection_matrix() noexcept;

// Allen K. Philbrick's asymmetric raw fusion: Mollweide north of the southern
// transition parallel, Sinusoidal south of it. Inputs and inverse outputs are
// coordinates on the authalic sphere. The fusion itself is equal-area.
[[nodiscard]] PlanarResult sinu_mollweide_forward(
    double longitude_rad,
    double authalic_latitude_rad,
    double radius_m = geo::authalic_radius_m()
) noexcept;

[[nodiscard]] SphericalResult sinu_mollweide_inverse(
    double x,
    double y,
    double radius_m = geo::authalic_radius_m()
) noexcept;

// WGS84-authalic oblique Philbrick aspect used as the mathematical basis for
// AERIS' Wide-Angle/Sinu-Mollweide surface. The ellipsoid is first mapped to
// its equal-area authalic sphere, then rigidly rotated so 55°N, 20°E is the
// projection-frame origin before the asymmetric fusion above is evaluated.
[[nodiscard]] PlanarResult philbrick_sinu_mollweide_forward_wgs84(
    double longitude_rad,
    double geodetic_latitude_rad,
    double radius_m = geo::authalic_radius_m()
) noexcept;

// Returns original WGS84 longitude/geodetic latitude. At a geographic pole the
// longitude remains explicitly indeterminate, matching the primitive inverse
// contracts.
[[nodiscard]] SphericalResult philbrick_sinu_mollweide_inverse_wgs84(
    double x,
    double y,
    double radius_m = geo::authalic_radius_m()
) noexcept;

}  // namespace aeris::projection
