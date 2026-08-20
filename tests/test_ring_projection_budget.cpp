// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/geometry/geographic.hpp"
#include "aeris/projection/ring.hpp"

#include "aeris/geo/wgs84.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iomanip>
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
    const aeris::geometry::RingInteriorSide interior_side =
        aeris::geometry::RingInteriorSide::unspecified
) {
    const auto result = aeris::geometry::canonicalize_wgs84_linear_ring(points);
    expect_true("verified projection input ring canonicalizes", result.ok());
    if (!result.ok()) return {};
    auto ring = result.value;
    ring.interior_side = interior_side;
    return ring;
}

[[nodiscard]] aeris::projection::RingProjectionOptions strict_projection_options(
    const aeris::projection::ProjectionAdapter& adapter,
    const double central_meridian,
    const unsigned max_rounds
) {
    aeris::projection::RingProjectionOptions options{};
    options.adapter = &adapter;
    options.central_meridian_rad = central_meridian;
    options.relative_area_tolerance = 1e-9;
    options.absolute_area_tolerance_m2 = 1.0;
    options.initial_geometric_tolerance_m = 8.0;
    options.initial_local_area_tolerance_m2 = 1024.0;
    options.max_refinement_rounds = max_rounds;
    return options;
}

void check_verified_projection(
    const aeris::geometry::LinearRing& ring,
    const aeris::projection::ProjectionAdapter& adapter,
    const double central_meridian,
    const unsigned max_rounds = 18U
) {
    const auto options = strict_projection_options(adapter, central_meridian, max_rounds);
    const auto result = aeris::projection::project_wgs84_linear_ring_verified(ring, options);

    expect_true("verified ring projection succeeds", result.ok());
    if (!result.ok()) {
        std::cerr << std::setprecision(17)
                  << "  adapter=" << adapter.descriptor().model_id
                  << " error=" << static_cast<int>(result.error)
                  << " geographic_error=" << static_cast<int>(result.geographic_error)
                  << " subdivision_error=" << static_cast<int>(result.subdivision_error)
                  << " sample_error=" << static_cast<int>(result.sample_error)
                  << " rounds=" << result.refinement_rounds
                  << " area_error=" << result.absolute_area_error_m2
                  << " allowed=" << result.allowed_area_error_m2 << '\n';
        return;
    }

    expect_true("verified result has finite source area", std::isfinite(result.source_signed_area_m2));
    expect_true("verified result has finite planar area", std::isfinite(result.planar_signed_area_m2));
    expect_true("verified result contains a polygon", result.projected_vertices >= 4U);
    expect_true(
        "verified result satisfies its published area budget",
        result.absolute_area_error_m2 <= result.allowed_area_error_m2
    );
}

void check_piecewise_projection(
    const aeris::geometry::LinearRing& ring,
    const aeris::projection::ProjectionAdapter& adapter,
    const std::size_t expected_pieces,
    const std::size_t expected_crossings,
    const unsigned max_rounds = 20U
) {
    auto options = strict_projection_options(adapter, 0.0, max_rounds);
    options.subdivision_max_depth = 32U;
    options.subdivision_max_segments_per_edge = 1'000'000U;
    options.max_projection_pieces = 64U;

    const auto result =
        aeris::projection::project_wgs84_linear_ring_piecewise_verified(ring, options);

    expect_true("piecewise verified projection succeeds", result.ok());
    if (!result.ok()) {
        std::cerr << std::setprecision(17)
                  << "  adapter=" << adapter.descriptor().model_id
                  << " error=" << static_cast<int>(result.error)
                  << " seam_error=" << static_cast<int>(result.seam_error)
                  << " piece_error=" << static_cast<int>(result.piece_error)
                  << " source_m2=" << result.source_signed_area_m2
                  << " planar_m2=" << result.planar_signed_area_m2
                  << " final_error_m2=" << result.absolute_area_error_m2
                  << " allowed_m2=" << result.allowed_area_error_m2
                  << " pieces=" << result.projected_pieces
                  << " crossings=" << result.seam_crossings << '\n';
        return;
    }

    expect_true("piecewise result has expected component count", result.projected_pieces == expected_pieces);
    expect_true("piecewise result reports expected seam crossings", result.seam_crossings == expected_crossings);
    expect_true("piecewise result stores every projected component", result.pieces.size() == expected_pieces);
    expect_true(
        "piecewise result satisfies original global area budget",
        result.absolute_area_error_m2 <= result.allowed_area_error_m2
    );
    for (const auto& piece : result.pieces) {
        expect_true("every projected piece is a polygon", piece.size() >= 4U);
    }
}

template <typename Callback>
void for_each_adapter(const Callback& callback) {
    for (const auto* adapter : aeris::projection::builtin_projection_adapters()) {
        expect_true("built-in adapter exists", adapter != nullptr);
        if (adapter != nullptr) callback(*adapter);
    }
}

void test_oblique_ring_budget() {
    const auto ring = make_ring({
        {radians(-35.0), radians(-20.0)},
        {radians(25.0), radians(-12.0)},
        {radians(40.0), radians(28.0)},
        {radians(-15.0), radians(42.0)},
    });
    for_each_adapter([&](const auto& adapter) {
        check_verified_projection(ring, adapter, 0.0);
    });
}

void test_antimeridian_ring_budget() {
    const auto ring = make_ring({
        {radians(170.0), radians(-20.0)},
        {radians(-170.0), radians(-20.0)},
        {radians(-170.0), radians(20.0)},
        {radians(170.0), radians(20.0)},
    });
    for_each_adapter([&](const auto& adapter) {
        check_verified_projection(ring, adapter, aeris::geo::kPi);
    });
}

void test_piecewise_two_crossing_antimeridian_budget() {
    const auto ring = make_ring(
        {
            {radians(170.0), radians(-20.0)},
            {radians(-170.0), radians(-20.0)},
            {radians(-170.0), radians(20.0)},
            {radians(170.0), radians(20.0)},
        },
        aeris::geometry::RingInteriorSide::left
    );
    for_each_adapter([&](const auto& adapter) {
        check_piecewise_projection(ring, adapter, 2U, 2U);
    });
}

void test_piecewise_four_crossing_concave_budget() {
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
    for_each_adapter([&](const auto& adapter) {
        check_piecewise_projection(ring, adapter, 3U, 4U);
    });
}

void test_piecewise_split_fails_closed_without_topology() {
    const auto ring = make_ring({
        {radians(170.0), radians(-20.0)},
        {radians(-170.0), radians(-20.0)},
        {radians(-170.0), radians(20.0)},
        {radians(170.0), radians(20.0)},
    });
    const auto result =
        aeris::projection::project_wgs84_linear_ring_piecewise_verified(ring);
    expect_true(
        "piecewise split without interior side fails closed",
        result.error == aeris::projection::PiecewiseRingProjectionError::seam_split_failed &&
        result.seam_error == aeris::projection::SeamSplitError::missing_interior_side
    );
}

void test_south_polar_budget() {
    const auto ring = make_ring(
        {
            {radians(-180.0), radians(-80.0)},
            {radians(-90.0), radians(-80.0)},
            {radians(0.0), radians(-80.0)},
            {radians(90.0), radians(-80.0)},
            {radians(180.0), radians(-80.0)},
        },
        aeris::geometry::RingInteriorSide::right
    );
    expect_true("south polar fixture winds once", ring.longitude_winding == 1);
    for_each_adapter([&](const auto& adapter) {
        check_verified_projection(ring, adapter, 0.0, 20U);
        check_piecewise_projection(ring, adapter, 1U, 0U, 20U);
    });
}

void test_impossible_budget_fails_closed() {
    const auto ring = make_ring({
        {radians(-35.0), radians(-20.0)},
        {radians(25.0), radians(-12.0)},
        {radians(40.0), radians(28.0)},
        {radians(-15.0), radians(42.0)},
    });

    aeris::projection::RingProjectionOptions options{};
    options.adapter = &aeris::projection::mollweide_projection_adapter();
    options.relative_area_tolerance = 0.0;
    options.absolute_area_tolerance_m2 = 0.001;
    options.initial_geometric_tolerance_m = 1000.0;
    options.initial_local_area_tolerance_m2 = 1e8;
    options.max_refinement_rounds = 1U;

    const auto result = aeris::projection::project_wgs84_linear_ring_verified(ring, options);
    expect_true(
        "unmet maximum-quality area budget fails closed",
        result.error == aeris::projection::RingProjectionError::area_budget_unmet
    );
}

void test_orientation_is_preserved() {
    std::vector<aeris::geometry::GeodeticPoint> points{
        {radians(-10.0), radians(-10.0)},
        {radians(20.0), radians(-10.0)},
        {radians(20.0), radians(20.0)},
        {radians(-10.0), radians(20.0)},
    };
    std::reverse(points.begin(), points.end());
    const auto ring = make_ring(points);

    for_each_adapter([&](const auto& adapter) {
        auto options = strict_projection_options(adapter, 0.0, 18U);
        const auto result = aeris::projection::project_wgs84_linear_ring_verified(ring, options);
        expect_true("reversed verified ring succeeds", result.ok());
        if (result.ok()) {
            expect_true("reversed source area remains negative", result.source_signed_area_m2 < 0.0);
            expect_true("reversed planar area remains negative", result.planar_signed_area_m2 < 0.0);
        }
    });
}

}  // namespace

int main() {
    test_oblique_ring_budget();
    test_antimeridian_ring_budget();
    test_piecewise_two_crossing_antimeridian_budget();
    test_piecewise_four_crossing_concave_budget();
    test_piecewise_split_fails_closed_without_topology();
    test_south_polar_budget();
    test_impossible_budget_fails_closed();
    test_orientation_is_preserved();

    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return EXIT_FAILURE;
    }

    std::cout << "ring_projection_budget: PASS\n";
    return EXIT_SUCCESS;
}
