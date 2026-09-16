// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/projection/ring.hpp"

#include "aeris/geo/wgs84.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

[[nodiscard]] double radians(const double degrees) noexcept {
    return degrees * aeris::geo::kPi / 180.0;
}

[[nodiscard]] aeris::geometry::LinearRing multi_crossing_south_polar_ring() {
    aeris::geometry::LinearRing ring{};
    ring.vertices = {
        {radians(180.0), radians(-80.0)},
        {radians(250.0), radians(-79.0)},
        {radians(200.0), radians(-78.0)},
        {radians(260.0), radians(-77.0)},
        {radians(300.0), radians(-79.0)},
        {radians(360.0), radians(-78.0)},
        {radians(420.0), radians(-79.0)},
        {radians(480.0), radians(-78.0)},
        {radians(538.0), radians(-80.0)},
    };
    ring.closing_longitude_rad = radians(540.0);
    ring.longitude_winding = 1;
    ring.interior_side = aeris::geometry::RingInteriorSide::right;
    return ring;
}

[[nodiscard]] aeris::projection::RingProjectionOptions options(
    const aeris::projection::EqualAreaPrimitive primitive
) {
    aeris::projection::RingProjectionOptions value{};
    value.primitive = primitive;
    value.central_meridian_rad = radians(37.0);
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
    const aeris::projection::EqualAreaPrimitive primitive,
    const std::string_view label
) {
    const auto ring = multi_crossing_south_polar_ring();
    const auto source_area = aeris::geometry::signed_wgs84_linear_ring_area(ring);
    if (!source_area.ok() || source_area.signed_area_m2 >= 0.0) {
        std::cerr << label << " fixture does not represent the intended south polar region\n";
        return false;
    }

    const auto result =
        aeris::projection::project_wgs84_linear_ring_piecewise_verified(
            ring,
            options(primitive)
        );
    if (!result.ok()) {
        std::cerr
            << label << " movable polar seam failed: error="
            << static_cast<unsigned>(result.error)
            << " piece_error=" << static_cast<unsigned>(result.piece_error)
            << " seam_error=" << static_cast<unsigned>(result.seam_error)
            << " failed_piece=" << result.failed_piece
            << " area_error=" << result.absolute_area_error_m2
            << " allowed=" << result.allowed_area_error_m2
            << '\n';
        return false;
    }

    if (result.projected_pieces < 2U ||
        result.source_signed_area_m2 >= 0.0 ||
        result.planar_signed_area_m2 >= 0.0 ||
        std::abs(result.source_signed_area_m2 - source_area.signed_area_m2) > 1e-3 ||
        result.absolute_area_error_m2 > result.allowed_area_error_m2) {
        std::cerr << label << " movable polar seam lost topology or area semantics\n";
        return false;
    }
    return true;
}

}  // namespace

int main() {
    if (!check(
            aeris::projection::EqualAreaPrimitive::sinusoidal,
            "Sinusoidal"
        ) ||
        !check(
            aeris::projection::EqualAreaPrimitive::mollweide,
            "Mollweide"
        )) {
        return EXIT_FAILURE;
    }

    std::cout << "polar_movable_seam: PASS\n";
    return EXIT_SUCCESS;
}
