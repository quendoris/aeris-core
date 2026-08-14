// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/projection/seam.hpp"

#include "aeris/geo/wgs84.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

[[nodiscard]] double radians(const double degrees) {
    return degrees * aeris::geo::kPi / 180.0;
}

void expect_true(const std::string_view name, const bool condition) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL " << name << '\n';
    }
}

[[nodiscard]] aeris::geometry::LinearRing make_ring(
    const std::vector<aeris::geometry::GeodeticPoint>& points,
    const aeris::geometry::RingInteriorSide side =
        aeris::geometry::RingInteriorSide::unspecified
) {
    auto result = aeris::geometry::canonicalize_wgs84_linear_ring(points);
    expect_true("seam fixture canonicalizes", result.ok());
    if (!result.ok()) {
        return {};
    }
    result.value.interior_side = side;
    return result.value;
}

void expect_piece_domain(
    const std::string_view name,
    const aeris::projection::SeamSplitResult& split,
    const double central_meridian
) {
    const double left = central_meridian - aeris::geo::kPi;
    const double right = central_meridian + aeris::geo::kPi;
    constexpr double tolerance = 1e-12;

    for (const auto& piece : split.pieces) {
        expect_true(name, piece.longitude_winding == 0);
        expect_true(name, piece.vertices.size() >= 3U);
        for (const auto point : piece.vertices) {
            expect_true(
                name,
                point.longitude_rad >= left - tolerance &&
                point.longitude_rad <= right + tolerance
            );
        }
        expect_true(
            name,
            piece.closing_longitude_rad >= left - tolerance &&
            piece.closing_longitude_rad <= right + tolerance
        );
    }
}

void expect_area_partition(
    const std::string_view name,
    const aeris::projection::SeamSplitResult& split
) {
    expect_true(name, split.ok());
    if (!split.ok()) {
        std::cerr
            << "  split_error=" << static_cast<int>(split.error)
            << " geographic_error=" << static_cast<int>(split.geographic_error)
            << " source_area=" << split.source_signed_area_m2
            << " piece_area=" << split.piece_signed_area_sum_m2
            << " abs_error=" << split.absolute_area_error_m2
            << " bound=" << split.area_error_bound_m2
            << '\n';
        return;
    }
    expect_true(
        name,
        split.absolute_area_error_m2 <= split.area_error_bound_m2
    );
}

void test_unsplit_branch_is_shifted_without_topology_guess() {
    const auto ring = make_ring({
        {radians(10.0), radians(-20.0)},
        {radians(40.0), radians(-20.0)},
        {radians(40.0), radians(20.0)},
        {radians(10.0), radians(20.0)},
    });

    aeris::projection::SeamSplitOptions options{};
    options.central_meridian_rad = -aeris::geo::kPi;
    const auto split = aeris::projection::split_wgs84_linear_ring_at_projection_seam(
        ring,
        options
    );

    expect_area_partition("unsplit shifted branch preserves area", split);
    if (!split.ok()) {
        return;
    }
    expect_true("unsplit ring remains one piece", split.pieces.size() == 1U);
    expect_true("unsplit ring reports no seam crossings", split.seam_crossings == 0U);
    expect_piece_domain("unsplit shifted branch lies in active domain", split, options.central_meridian_rad);
}

void test_two_crossing_antimeridian_rectangle() {
    const auto ring = make_ring(
        {
            {radians(170.0), radians(-20.0)},
            {radians(-170.0), radians(-20.0)},
            {radians(-170.0), radians(20.0)},
            {radians(170.0), radians(20.0)},
        },
        aeris::geometry::RingInteriorSide::left
    );

    const auto split = aeris::projection::split_wgs84_linear_ring_at_projection_seam(ring);
    expect_area_partition("two-crossing rectangle preserves area", split);
    if (!split.ok()) {
        return;
    }

    expect_true("two-crossing rectangle becomes two pieces", split.pieces.size() == 2U);
    expect_true("two-crossing rectangle exposes two crossings", split.seam_crossings == 2U);
    expect_piece_domain("two-crossing pieces lie in active domain", split, 0.0);

    for (const auto& piece : split.pieces) {
        const auto area = aeris::geometry::signed_wgs84_linear_ring_area(piece);
        expect_true("two-crossing piece area succeeds", area.ok());
        if (area.ok()) {
            expect_true("left-side rectangle pieces preserve positive orientation", area.signed_area_m2 > 0.0);
        }
    }
}

void test_reversed_two_crossing_rectangle() {
    std::vector<aeris::geometry::GeodeticPoint> points{
        {radians(170.0), radians(-20.0)},
        {radians(-170.0), radians(-20.0)},
        {radians(-170.0), radians(20.0)},
        {radians(170.0), radians(20.0)},
    };
    std::reverse(points.begin(), points.end());
    const auto ring = make_ring(
        points,
        aeris::geometry::RingInteriorSide::right
    );

    const auto split = aeris::projection::split_wgs84_linear_ring_at_projection_seam(ring);
    expect_area_partition("reversed rectangle preserves area", split);
    if (!split.ok()) {
        return;
    }

    expect_true("reversed rectangle becomes two pieces", split.pieces.size() == 2U);
    for (const auto& piece : split.pieces) {
        const auto area = aeris::geometry::signed_wgs84_linear_ring_area(piece);
        expect_true("reversed piece area succeeds", area.ok());
        if (area.ok()) {
            expect_true("right-side pieces preserve negative orientation", area.signed_area_m2 < 0.0);
        }
    }
}

void test_four_crossings_produce_three_components() {
    // Concave simple polygon in the unwrapped longitude plane:
    //
    //   160 ---------------- 200
    //    |                    |
    //    |         170 ------ 200
    //    |         |
    //    |         170 ------ 200
    //    |                    |
    //   160 ---------------- 200
    //
    // The active +180 seam intersects it four times. The west side remains
    // one connected component while the east side becomes two disconnected
    // components. A naive "one piece per boundary arc" algorithm would return
    // four pieces and is therefore wrong.
    const auto ring = make_ring(
        {
            {radians(160.0), radians(-50.0)},
            {radians(-160.0), radians(-50.0)},
            {radians(-160.0), radians(-20.0)},
            {radians(170.0), radians(-20.0)},
            {radians(170.0), radians(20.0)},
            {radians(-160.0), radians(20.0)},
            {radians(-160.0), radians(50.0)},
            {radians(160.0), radians(50.0)},
        },
        aeris::geometry::RingInteriorSide::left
    );

    const auto split = aeris::projection::split_wgs84_linear_ring_at_projection_seam(ring);
    expect_area_partition("four-crossing concave polygon preserves area", split);
    if (!split.ok()) {
        return;
    }

    expect_true("concave polygon exposes four crossings", split.seam_crossings == 4U);
    expect_true("four crossings form three connected pieces", split.pieces.size() == 3U);
    expect_piece_domain("concave split pieces lie in active domain", split, 0.0);
}

void test_split_requires_explicit_interior_side() {
    const auto ring = make_ring({
        {radians(170.0), radians(-20.0)},
        {radians(-170.0), radians(-20.0)},
        {radians(-170.0), radians(20.0)},
        {radians(170.0), radians(20.0)},
    });

    const auto split = aeris::projection::split_wgs84_linear_ring_at_projection_seam(ring);
    expect_true(
        "seam split without interior topology fails closed",
        split.error == aeris::projection::SeamSplitError::missing_interior_side
    );
}

void test_nonzero_winding_is_not_stolen_from_polar_contract() {
    auto ring = make_ring(
        {
            {radians(-180.0), radians(-80.0)},
            {radians(-90.0), radians(-80.0)},
            {radians(0.0), radians(-80.0)},
            {radians(90.0), radians(-80.0)},
            {radians(180.0), radians(-80.0)},
        },
        aeris::geometry::RingInteriorSide::right
    );
    expect_true("polar seam fixture winds once", ring.longitude_winding == 1);

    const auto split = aeris::projection::split_wgs84_linear_ring_at_projection_seam(ring);
    expect_true(
        "general zero-winding splitter refuses polar topology",
        split.error == aeris::projection::SeamSplitError::nonzero_winding_unsupported
    );
}

}  // namespace

int main() {
    test_unsplit_branch_is_shifted_without_topology_guess();
    test_two_crossing_antimeridian_rectangle();
    test_reversed_two_crossing_rectangle();
    test_four_crossings_produce_three_components();
    test_split_requires_explicit_interior_side();
    test_nonzero_winding_is_not_stolen_from_polar_contract();

    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return EXIT_FAILURE;
    }

    std::cout << "projection_seam_split: PASS\n";
    return EXIT_SUCCESS;
}
