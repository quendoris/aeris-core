// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "aeris/projection/ring.hpp"

#include <cstddef>
#include <vector>

namespace aeris::projection {

enum class SinuMollweideRingError {
    none = 0,
    invalid_options,
    source_area_failed,
    frame_transform_failed,
    frame_area_budget_unmet,
    projection_failed,
    aggregate_area_budget_unmet,
};

struct SinuMollweideRingProjectionResult final {
    std::vector<std::vector<geometry::PlanarPoint>> pieces;

    double source_signed_area_m2 = 0.0;
    double frame_signed_area_m2 = 0.0;
    double planar_signed_area_m2 = 0.0;
    double frame_absolute_area_error_m2 = 0.0;
    double absolute_area_error_m2 = 0.0;
    double allowed_area_error_m2 = 0.0;

    std::size_t seam_crossings = 0U;
    std::size_t projected_vertices = 0U;
    unsigned frame_refinement_rounds = 0U;
    unsigned max_projection_refinement_rounds = 0U;

    SinuMollweideRingError error = SinuMollweideRingError::none;
    PiecewiseRingProjectionError projection_error =
        PiecewiseRingProjectionError::none;
    RingProjectionError piece_error = RingProjectionError::none;
    SeamSplitError seam_error = SeamSplitError::none;
    geometry::GeographicError geographic_error = geometry::GeographicError::none;
    SubdivisionError subdivision_error = SubdivisionError::none;
    geo::MathError sample_error = geo::MathError::none;
    std::size_t failed_piece = 0U;
    std::size_t failed_edge = 0U;

    [[nodiscard]] bool ok() const noexcept {
        return error == SinuMollweideRingError::none;
    }
};

// Verified projection of one canonical WGS84-linear ring through the default
// Philbrick oblique Sinu-Mollweide frame.
//
// The source ring is never reinterpreted as a different linear geometry. Its
// exact WGS84 area is measured first. The authalic-sphere rotation is then
// adaptively approximated by a pseudo-WGS84 polyline whose authalic latitudes
// exactly represent the rotated frame. That approximation receives only a
// bounded fraction of the caller's total area budget. The existing strict
// seam/polar topology and verified ring projector then operate on that frame
// ring with EqualAreaPrimitive::sinu_mollweide. Finally the sum of all planar
// pieces is checked again against the original source-ring area.
//
// options.central_meridian_rad is interpreted inside the Philbrick projection
// frame. Changing it moves the single cut without changing the equal-area
// surface or requiring inset maps. options.primitive is ignored and forced to
// EqualAreaPrimitive::sinu_mollweide.
[[nodiscard]] SinuMollweideRingProjectionResult
project_philbrick_wgs84_linear_ring_piecewise_verified(
    const geometry::LinearRing& ring,
    const RingProjectionOptions& options = {}
);

}  // namespace aeris::projection
