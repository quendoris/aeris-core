// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/projection/ring.hpp"

#include "aeris/geo/wgs84.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>

namespace {

[[nodiscard]] double radians(const double degrees) noexcept {
    return degrees * aeris::geo::kPi / 180.0;
}

[[nodiscard]] aeris::geometry::LinearRing natural_earth_style_south_polar_ring() {
    const double seam = aeris::geo::kPi;
    const double pole_side = std::nextafter(seam, std::numeric_limits<double>::infinity());

    aeris::geometry::LinearRing ring{};
    ring.vertices = {
        {seam, radians(-80.0)},
        {seam, -aeris::geo::kHalfPi},
        {pole_side, -aeris::geo::kHalfPi},
        {pole_side, radians(-80.0)},
        {radians(240.0), radians(-78.0)},
        {radians(300.0), radians(-79.0)},
        {radians(360.0), radians(-77.0)},
        {radians(420.0), radians(-79.0)},
        {radians(480.0), radians(-78.0)},
        {radians(538.0), radians(-80.0)},
    };

    // A long canonicalized real-world ring accumulates binary64 longitude
    // roundoff. Its topological closure is the +pi seam one turn later, but the
    // stored closing longitude can land a few ulps short of exactly 3*pi.
    ring.closing_longitude_rad =
        3.0 * aeris::geo::kPi -
        8.0 * std::numeric_limits<double>::epsilon() * aeris::geo::kPi;
    ring.longitude_winding = 1;
    ring.interior_side = aeris::geometry::RingInteriorSide::right;
    return ring;
}

[[nodiscard]] aeris::projection::RingProjectionOptions options(
    const aeris::projection::EqualAreaPrimitive primitive
) {
    aeris::projection::RingProjectionOptions value{};
    value.primitive = primitive;
    value.central_meridian_rad = 0.0;
    value.relative_area_tolerance = 1e-7;
    value.absolute_area_tolerance_m2 = 10'000.0;
    value.initial_geometric_tolerance_m = 2'000.0;
    value.initial_local_area_tolerance_m2 = 1.0e8;
    value.max_refinement_rounds = 18U;
    value.subdivision_max_depth = 32U;
    value.subdivision_max_segments_per_edge = 1'000'000U;
    value.max_projection_pieces = 4096U;
    return value;
}

[[nodiscard]] bool check(
    const aeris::geometry::LinearRing& ring,
    const aeris::projection::EqualAreaPrimitive primitive,
    const std::string_view label
) {
    const double final_edge_delta =
        ring.closing_longitude_rad - ring.vertices.back().longitude_rad;
    const double legacy_parameter =
        (3.0 * aeris::geo::kPi - ring.vertices.back().longitude_rad) /
        final_edge_delta;
    constexpr double legacy_parameter_tolerance =
        64.0 * std::numeric_limits<double>::epsilon();
    if (!(legacy_parameter > 1.0 + legacy_parameter_tolerance)) {
        std::cerr << label << " fixture does not exercise seam endpoint roundoff\n";
        return false;
    }

    const auto result =
        aeris::projection::project_wgs84_linear_ring_piecewise_verified(
            ring,
            options(primitive)
        );
    if (!result.ok()) {
        std::cerr
            << label
            << " polar seam endpoint projection failed: error="
            << static_cast<unsigned>(result.error)
            << " piece_error=" << static_cast<unsigned>(result.piece_error)
            << " seam_error=" << static_cast<unsigned>(result.seam_error)
            << " area_error=" << result.absolute_area_error_m2
            << " allowed=" << result.allowed_area_error_m2
            << '\n';
        return false;
    }

    if (result.projected_pieces != 1U ||
        result.source_signed_area_m2 >= 0.0 ||
        result.planar_signed_area_m2 >= 0.0 ||
        result.absolute_area_error_m2 > result.allowed_area_error_m2) {
        std::cerr << label << " polar semantics were not preserved\n";
        return false;
    }
    return true;
}

}  // namespace

int main() {
    const auto ring = natural_earth_style_south_polar_ring();
    if (!check(
            ring,
            aeris::projection::EqualAreaPrimitive::sinusoidal,
            "Sinusoidal"
        ) ||
        !check(
            ring,
            aeris::projection::EqualAreaPrimitive::mollweide,
            "Mollweide"
        )) {
        return EXIT_FAILURE;
    }

    std::cout << "polar_seam_roundoff: PASS\n";
    return EXIT_SUCCESS;
}
