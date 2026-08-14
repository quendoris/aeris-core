// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "aeris/geometry/geographic.hpp"

#include <cstddef>
#include <vector>

namespace aeris::projection {

enum class SeamSplitError {
    none = 0,
    invalid_options,
    invalid_ring,
    nonzero_winding_unsupported,
    missing_interior_side,
    seam_coincident_edge,
    ambiguous_seam_touch,
    topology_inconsistent,
    piece_limit_exceeded,
    geographic_area_failed,
    area_invariant_failed,
};

struct SeamSplitOptions final {
    double central_meridian_rad = 0.0;
    std::size_t max_pieces = 4096U;
};

struct SeamSplitResult final {
    std::vector<geometry::LinearRing> pieces;

    std::size_t seam_crossings = 0U;
    double source_signed_area_m2 = 0.0;
    double piece_signed_area_sum_m2 = 0.0;
    double absolute_area_error_m2 = 0.0;
    double area_error_bound_m2 = 0.0;

    SeamSplitError error = SeamSplitError::none;
    geometry::GeographicError geographic_error = geometry::GeographicError::none;

    [[nodiscard]] bool ok() const noexcept {
        return error == SeamSplitError::none;
    }
};

// Split a zero-longitude-winding canonical WGS84 ring at the active projection
// seam. Returned pieces are shifted by whole longitude turns into the active
// projection domain [central_meridian - pi, central_meridian + pi].
//
// If the complete ring already fits one longitude branch, one shifted piece is
// returned and explicit interior-side topology is not required. If an actual
// split is necessary, RingInteriorSide must be known so seam connectors can be
// oriented without guessing the intended spherical/ellipsoidal region.
[[nodiscard]] SeamSplitResult split_wgs84_linear_ring_at_projection_seam(
    const geometry::LinearRing& ring,
    const SeamSplitOptions& options = {}
);

}  // namespace aeris::projection
