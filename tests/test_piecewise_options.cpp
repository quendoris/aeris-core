// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/projection/ring.hpp"

#include "aeris/geo/wgs84.hpp"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <vector>

namespace {

[[nodiscard]] double radians(const double degrees) {
    return degrees * aeris::geo::kPi / 180.0;
}

[[nodiscard]] aeris::geometry::LinearRing seam_crossing_ring() {
    const std::vector<aeris::geometry::GeodeticPoint> points{
        {radians(170.0), radians(-20.0)},
        {radians(-170.0), radians(-20.0)},
        {radians(-170.0), radians(20.0)},
        {radians(170.0), radians(20.0)},
    };
    auto canonical = aeris::geometry::canonicalize_wgs84_linear_ring(points);
    if (!canonical.ok()) {
        return {};
    }
    canonical.value.interior_side = aeris::geometry::RingInteriorSide::left;
    return canonical.value;
}

[[nodiscard]] bool rejected_as_invalid(
    const aeris::geometry::LinearRing& ring,
    const aeris::projection::RingProjectionOptions& options
) {
    const auto result =
        aeris::projection::project_wgs84_linear_ring_piecewise_verified(
            ring,
            options
        );
    return result.error ==
        aeris::projection::PiecewiseRingProjectionError::invalid_options;
}

}  // namespace

int main() {
    const auto ring = seam_crossing_ring();
    if (ring.vertices.empty()) {
        std::cerr << "failed to construct piecewise option fixture\n";
        return EXIT_FAILURE;
    }

    aeris::projection::RingProjectionOptions options{};

    options.initial_geometric_tolerance_m = 0.0;
    if (!rejected_as_invalid(ring, options)) {
        std::cerr << "zero geometric tolerance was not rejected at API boundary\n";
        return EXIT_FAILURE;
    }

    options = {};
    options.initial_local_area_tolerance_m2 = 0.0;
    if (!rejected_as_invalid(ring, options)) {
        std::cerr << "zero local-area tolerance was not rejected at API boundary\n";
        return EXIT_FAILURE;
    }

    options = {};
    options.max_refinement_rounds = 0U;
    if (!rejected_as_invalid(ring, options)) {
        std::cerr << "zero refinement rounds were not rejected at API boundary\n";
        return EXIT_FAILURE;
    }

    options = {};
    options.subdivision_max_depth = 0U;
    if (!rejected_as_invalid(ring, options)) {
        std::cerr << "zero subdivision depth was not rejected at API boundary\n";
        return EXIT_FAILURE;
    }

    options = {};
    options.subdivision_max_segments_per_edge = 0U;
    if (!rejected_as_invalid(ring, options)) {
        std::cerr << "zero segment limit was not rejected at API boundary\n";
        return EXIT_FAILURE;
    }

    options = {};
    options.max_projection_pieces = 0U;
    if (!rejected_as_invalid(ring, options)) {
        std::cerr << "zero piece limit was not rejected at API boundary\n";
        return EXIT_FAILURE;
    }

    options = {};
    options.central_meridian_rad = std::numeric_limits<double>::quiet_NaN();
    if (!rejected_as_invalid(ring, options)) {
        std::cerr << "non-finite central meridian was not rejected at API boundary\n";
        return EXIT_FAILURE;
    }

    std::cout << "piecewise_options: PASS\n";
    return EXIT_SUCCESS;
}
