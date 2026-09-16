// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/projection/sinu_mollweide.hpp"
#include "aeris/projection/sinu_mollweide_ring.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void expect_true(const std::string_view name, const bool condition) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL " << name << '\n';
    }
}

[[nodiscard]] double radians(const double degrees) noexcept {
    return degrees * aeris::geo::kPi / 180.0;
}

[[nodiscard]] aeris::geometry::GeodeticPoint world_from_frame(
    const double frame_longitude_deg,
    const double frame_geodetic_latitude_deg
) {
    const auto beta = aeris::geo::authalic_latitude(
        radians(frame_geodetic_latitude_deg)
    );
    expect_true("frame fixture authalic latitude succeeds", beta.ok());
    if (!beta.ok()) return {};

    const auto planar = aeris::projection::sinu_mollweide_forward(
        radians(frame_longitude_deg),
        beta.value
    );
    expect_true("frame fixture raw projection succeeds", planar.ok());
    if (!planar.ok()) return {};

    const auto world = aeris::projection::philbrick_sinu_mollweide_inverse_wgs84(
        planar.value.x,
        planar.value.y
    );
    expect_true("frame fixture inverse to WGS84 succeeds", world.ok());
    if (!world.ok()) return {};
    return {world.value.longitude_rad, world.value.latitude_rad};
}

[[nodiscard]] aeris::geometry::LinearRing make_world_ring_from_frame(
    const std::vector<aeris::geometry::GeodeticPoint>& frame_degrees,
    const aeris::geometry::RingInteriorSide side
) {
    std::vector<aeris::geometry::GeodeticPoint> world;
    world.reserve(frame_degrees.size());
    for (const auto point : frame_degrees) {
        world.push_back(world_from_frame(point.longitude_rad, point.latitude_rad));
    }

    auto canonical = aeris::geometry::canonicalize_wgs84_linear_ring(world);
    expect_true("world fixture canonicalizes", canonical.ok());
    if (!canonical.ok()) return {};
    canonical.value.interior_side = side;
    return canonical.value;
}

[[nodiscard]] aeris::projection::RingProjectionOptions verified_options() {
    aeris::projection::RingProjectionOptions options{};
    options.relative_area_tolerance = 1e-7;
    options.absolute_area_tolerance_m2 = 10'000.0;
    options.initial_geometric_tolerance_m = 2'000.0;
    options.initial_local_area_tolerance_m2 = 1.0e8;
    options.max_refinement_rounds = 18U;
    options.subdivision_max_depth = 32U;
    options.subdivision_max_segments_per_edge = 1'000'000U;
    options.max_projection_pieces = 4096U;
    return options;
}

void expect_verified_area(
    const std::string_view name,
    const aeris::projection::SinuMollweideRingProjectionResult& result
) {
    expect_true(name, result.ok());
    if (!result.ok()) {
        std::cerr
            << "  error=" << static_cast<unsigned>(result.error)
            << " projection=" << static_cast<unsigned>(result.projection_error)
            << " piece=" << static_cast<unsigned>(result.piece_error)
            << " seam=" << static_cast<unsigned>(result.seam_error)
            << " geographic=" << static_cast<unsigned>(result.geographic_error)
            << " subdivision=" << static_cast<unsigned>(result.subdivision_error)
            << " sample=" << static_cast<unsigned>(result.sample_error)
            << " source_area=" << result.source_signed_area_m2
            << " frame_area=" << result.frame_signed_area_m2
            << " planar_area=" << result.planar_signed_area_m2
            << " frame_error=" << result.frame_absolute_area_error_m2
            << " final_error=" << result.absolute_area_error_m2
            << " allowed=" << result.allowed_area_error_m2
            << '\n';
        return;
    }

    expect_true(
        name,
        std::isfinite(result.absolute_area_error_m2) &&
        result.absolute_area_error_m2 <= result.allowed_area_error_m2
    );
    expect_true(name, !result.pieces.empty());
    for (const auto& piece : result.pieces) {
        expect_true(name, piece.size() >= 3U);
        for (const auto point : piece) {
            expect_true(name, std::isfinite(point.x) && std::isfinite(point.y));
        }
    }
}

void test_unsplit_single_surface_ring() {
    const auto ring = make_world_ring_from_frame(
        {
            {-25.0, -15.0},
            {25.0, -15.0},
            {25.0, 15.0},
            {-25.0, 15.0},
        },
        aeris::geometry::RingInteriorSide::left
    );

    const auto result =
        aeris::projection::project_philbrick_wgs84_linear_ring_piecewise_verified(
            ring,
            verified_options()
        );
    expect_verified_area("ordinary Philbrick ring verifies", result);
    if (result.ok()) {
        expect_true("ordinary Philbrick ring remains one piece", result.pieces.size() == 1U);
    }
}

[[nodiscard]] aeris::geometry::LinearRing make_local_seam_crossing_ring() {
    // The source contract is WGS84-linear, not projection-frame-linear. Keep the
    // two sides only one degree apart across the physical +/-180 frame seam so
    // the inverse-mapped WGS84 endpoints are local neighbours and their source
    // linear edges genuinely cross that same oblique cut. A broad +/-170 frame
    // fixture only constrained its vertices; after inverse mapping, its WGS84
    // linear edges followed a different curved path through the projection frame.
    return make_world_ring_from_frame(
        {
            {179.5, -5.0},
            {-179.5, -5.0},
            {-179.5, 5.0},
            {179.5, 5.0},
        },
        aeris::geometry::RingInteriorSide::left
    );
}

void test_single_cut_splits_but_does_not_create_a_second_map() {
    const auto ring = make_local_seam_crossing_ring();

    const auto result =
        aeris::projection::project_philbrick_wgs84_linear_ring_piecewise_verified(
            ring,
            verified_options()
        );
    expect_verified_area("seam-crossing Philbrick ring verifies", result);
    if (result.ok()) {
        // `pieces` and the original WGS84 area are normative. The legacy seam
        // incidence counter deliberately does not count a crossing sampled
        // exactly at a splitter vertex; hardening that diagnostic is a separate
        // compatibility change before seam statistics are exposed to a UI.
        expect_true("single source ring becomes two planar pieces", result.pieces.size() == 2U);
    }
}

void test_moving_cut_changes_piece_topology_not_surface_semantics() {
    const auto ring = make_local_seam_crossing_ring();

    auto moved_options = verified_options();
    moved_options.central_meridian_rad = radians(40.0);
    const auto moved =
        aeris::projection::project_philbrick_wgs84_linear_ring_piecewise_verified(
            ring,
            moved_options
        );
    expect_verified_area("moved-cut Philbrick ring verifies", moved);
    if (moved.ok()) {
        expect_true("moved cut keeps one planar piece", moved.pieces.size() == 1U);
    }
}

}  // namespace

int main() {
    test_unsplit_single_surface_ring();
    test_single_cut_splits_but_does_not_create_a_second_map();
    test_moving_cut_changes_piece_topology_not_surface_semantics();

    if (failures != 0) {
        std::cerr << failures << " Sinu-Mollweide ring assertion(s) failed\n";
        return EXIT_FAILURE;
    }

    std::cout << "sinu_mollweide_ring: PASS\n";
    return EXIT_SUCCESS;
}
