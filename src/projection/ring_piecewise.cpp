// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/projection/ring.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

namespace aeris::projection {
namespace {

[[nodiscard]] bool valid_piecewise_options(
    const RingProjectionOptions& options
) noexcept {
    return std::isfinite(options.central_meridian_rad) &&
           std::isfinite(options.relative_area_tolerance) &&
           options.relative_area_tolerance >= 0.0 &&
           std::isfinite(options.absolute_area_tolerance_m2) &&
           options.absolute_area_tolerance_m2 > 0.0 &&
           std::isfinite(options.initial_geometric_tolerance_m) &&
           options.initial_geometric_tolerance_m > 0.0 &&
           std::isfinite(options.initial_local_area_tolerance_m2) &&
           options.initial_local_area_tolerance_m2 > 0.0 &&
           options.max_refinement_rounds > 0U &&
           options.subdivision_max_depth > 0U &&
           options.subdivision_max_segments_per_edge > 0U &&
           options.max_projection_pieces > 0U;
}

void copy_piece_failure(
    const RingProjectionResult& piece,
    const std::size_t piece_index,
    PiecewiseRingProjectionResult& result
) noexcept {
    result.error = PiecewiseRingProjectionError::piece_projection_failed;
    result.piece_error = piece.error;
    result.geographic_error = piece.geographic_error;
    result.subdivision_error = piece.subdivision_error;
    result.sample_error = piece.sample_error;
    result.failed_piece = piece_index;
    result.failed_edge = piece.failed_edge;
}

[[nodiscard]] bool orientation_changed(
    const double source,
    const double planar
) noexcept {
    if (source == 0.0 || planar == 0.0) {
        return false;
    }
    return (source < 0.0) != (planar < 0.0);
}

void adopt_single_piece(
    RingProjectionResult&& single,
    PiecewiseRingProjectionResult& result
) {
    if (!single.ok()) {
        copy_piece_failure(single, 0U, result);
        return;
    }

    if (orientation_changed(
            single.source_signed_area_m2,
            single.planar_signed_area_m2
        )) {
        result.error = PiecewiseRingProjectionError::piece_orientation_changed;
        result.failed_piece = 0U;
        return;
    }

    result.source_signed_area_m2 = single.source_signed_area_m2;
    result.planar_signed_area_m2 = single.planar_signed_area_m2;
    result.absolute_area_error_m2 = single.absolute_area_error_m2;
    result.allowed_area_error_m2 = single.allowed_area_error_m2;
    result.projected_vertices = single.projected_vertices;
    result.projected_pieces = 1U;
    result.max_piece_refinement_rounds = single.refinement_rounds;
    result.pieces.push_back(std::move(single.points));
}

[[nodiscard]] bool make_explicit_polar_closure(
    const geometry::LinearRing& ring,
    geometry::LinearRing& closed
) {
    if ((ring.longitude_winding != 1 && ring.longitude_winding != -1) ||
        ring.interior_side == geometry::RingInteriorSide::unspecified ||
        ring.vertices.empty() ||
        !std::isfinite(ring.closing_longitude_rad)) {
        return false;
    }

    const double winding_sign = ring.longitude_winding > 0 ? 1.0 : -1.0;
    const double pole_sign =
        ring.interior_side == geometry::RingInteriorSide::left
            ? winding_sign
            : -winding_sign;
    const double pole_latitude = pole_sign * geo::kHalfPi;

    // A winding ring is a closed curve on the sphere but remains open in the
    // canonical unwrapped longitude plane by exactly one turn. Materialize the
    // selected spherical interior as an ordinary zero-winding ring: continue
    // from the topological closing point to the selected pole, traverse the
    // degenerate pole longitude back one turn, then let the ordinary closing
    // edge return from that pole to the first source vertex.
    //
    // The pole-longitude edge is not a visual artefact. In the geographic area
    // integral it contributes exactly the topology term that selects the polar
    // side of a winding ring. Once explicit, the existing strip splitter can
    // partition any number of physical crossings of an arbitrary active seam.
    closed = {};
    closed.vertices.reserve(ring.vertices.size() + 3U);
    closed.vertices.insert(
        closed.vertices.end(),
        ring.vertices.begin(),
        ring.vertices.end()
    );

    const geometry::GeodeticPoint topological_close{
        ring.closing_longitude_rad,
        ring.vertices.front().latitude_rad,
    };
    const geometry::GeodeticPoint last = closed.vertices.back();
    if (last.longitude_rad != topological_close.longitude_rad ||
        last.latitude_rad != topological_close.latitude_rad) {
        closed.vertices.push_back(topological_close);
    }

    closed.vertices.push_back({ring.closing_longitude_rad, pole_latitude});
    closed.vertices.push_back({ring.vertices.front().longitude_rad, pole_latitude});
    closed.closing_longitude_rad = ring.vertices.front().longitude_rad;
    closed.longitude_winding = 0;
    closed.interior_side = ring.interior_side;
    return true;
}

}  // namespace

PiecewiseRingProjectionResult project_wgs84_linear_ring_piecewise_verified(
    const geometry::LinearRing& ring,
    const RingProjectionOptions& options
) {
    PiecewiseRingProjectionResult result{};

    if (!valid_piecewise_options(options)) {
        result.error = PiecewiseRingProjectionError::invalid_options;
        return result;
    }

    geometry::LinearRing split_ring = ring;
    double polar_closure_error_m2 = 0.0;
    geometry::GeographicAreaResult original_polar_area{};

    if (ring.longitude_winding != 0) {
        // Keep the legacy single-branch projector as the explicit unsupported
        // fallback for winding magnitudes outside the canonical polar contract.
        if (!make_explicit_polar_closure(ring, split_ring)) {
            adopt_single_piece(
                project_wgs84_linear_ring_verified(ring, options),
                result
            );
            return result;
        }

        original_polar_area = geometry::signed_wgs84_linear_ring_area(ring);
        const geometry::GeographicAreaResult closed_area =
            geometry::signed_wgs84_linear_ring_area(split_ring);
        if (!original_polar_area.ok() || !closed_area.ok()) {
            result.error = PiecewiseRingProjectionError::geographic_area_failed;
            result.geographic_error = !original_polar_area.ok()
                ? original_polar_area.error
                : closed_area.error;
            return result;
        }

        polar_closure_error_m2 = std::abs(
            closed_area.signed_area_m2 - original_polar_area.signed_area_m2
        );
        if (!std::isfinite(polar_closure_error_m2)) {
            result.error = PiecewiseRingProjectionError::non_finite_planar_area;
            return result;
        }
    }

    SeamSplitOptions split_options{};
    split_options.central_meridian_rad = options.central_meridian_rad;
    split_options.max_pieces = options.max_projection_pieces;
    SeamSplitResult split = split_wgs84_linear_ring_at_projection_seam(
        split_ring,
        split_options
    );
    if (!split.ok()) {
        result.error = PiecewiseRingProjectionError::seam_split_failed;
        result.seam_error = split.error;
        result.geographic_error = split.geographic_error;
        return result;
    }
    if (split.pieces.empty() ||
        split.pieces.size() > options.max_projection_pieces) {
        result.error = PiecewiseRingProjectionError::seam_split_failed;
        result.seam_error = SeamSplitError::piece_limit_exceeded;
        return result;
    }

    result.source_signed_area_m2 = ring.longitude_winding == 0
        ? split.source_signed_area_m2
        : original_polar_area.signed_area_m2;
    result.seam_partition_error_m2 =
        split.absolute_area_error_m2 + polar_closure_error_m2;
    result.seam_crossings = split.seam_crossings;
    result.allowed_area_error_m2 = std::max(
        options.absolute_area_tolerance_m2,
        options.relative_area_tolerance *
            std::abs(result.source_signed_area_m2)
    );
    if (!std::isfinite(result.allowed_area_error_m2) ||
        !std::isfinite(result.seam_partition_error_m2)) {
        result.error = PiecewiseRingProjectionError::invalid_options;
        return result;
    }

    const double projection_budget =
        result.allowed_area_error_m2 - result.seam_partition_error_m2;
    if (!std::isfinite(projection_budget) || projection_budget <= 0.0) {
        result.error = PiecewiseRingProjectionError::aggregate_area_budget_unmet;
        result.absolute_area_error_m2 = result.seam_partition_error_m2;
        return result;
    }

    const double piece_budget =
        projection_budget / static_cast<double>(split.pieces.size());
    if (!std::isfinite(piece_budget) || piece_budget <= 0.0) {
        result.error = PiecewiseRingProjectionError::invalid_options;
        return result;
    }

    RingProjectionOptions piece_options = options;
    piece_options.relative_area_tolerance = 0.0;
    piece_options.absolute_area_tolerance_m2 = piece_budget;

    result.pieces.reserve(split.pieces.size());
    long double planar_sum = 0.0L;

    for (std::size_t piece_index = 0U;
         piece_index < split.pieces.size();
         ++piece_index) {
        RingProjectionResult projected = project_wgs84_linear_ring_verified(
            split.pieces[piece_index],
            piece_options
        );
        if (!projected.ok()) {
            copy_piece_failure(projected, piece_index, result);
            return result;
        }

        if (orientation_changed(
                projected.source_signed_area_m2,
                projected.planar_signed_area_m2
            ) ||
            orientation_changed(
                result.source_signed_area_m2,
                projected.source_signed_area_m2
            )) {
            result.error = PiecewiseRingProjectionError::piece_orientation_changed;
            result.failed_piece = piece_index;
            return result;
        }

        planar_sum += static_cast<long double>(projected.planar_signed_area_m2);
        result.projected_vertices += projected.projected_vertices;
        result.max_piece_refinement_rounds = std::max(
            result.max_piece_refinement_rounds,
            projected.refinement_rounds
        );
        result.pieces.push_back(std::move(projected.points));
    }

    result.projected_pieces = result.pieces.size();
    result.planar_signed_area_m2 = static_cast<double>(planar_sum);
    if (!std::isfinite(result.planar_signed_area_m2)) {
        result.error = PiecewiseRingProjectionError::non_finite_planar_area;
        return result;
    }

    result.absolute_area_error_m2 = std::abs(
        result.planar_signed_area_m2 - result.source_signed_area_m2
    );
    if (!std::isfinite(result.absolute_area_error_m2)) {
        result.error = PiecewiseRingProjectionError::non_finite_planar_area;
        return result;
    }
    if (result.absolute_area_error_m2 > result.allowed_area_error_m2) {
        result.error = PiecewiseRingProjectionError::aggregate_area_budget_unmet;
        return result;
    }

    return result;
}

}  // namespace aeris::projection
