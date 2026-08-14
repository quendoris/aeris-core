// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "aeris/geometry/geographic.hpp"
#include "aeris/geometry/planar.hpp"
#include "aeris/projection/subdivide.hpp"
#include "aeris/projection/wgs84.hpp"

#include <cstddef>
#include <vector>

namespace aeris::projection {

enum class RingProjectionError {
    none = 0,
    invalid_options,
    geographic_area_failed,
    subdivision_failed,
    non_finite_planar_area,
    area_budget_unmet,
    unsupported_seam_topology,
};

struct RingProjectionOptions final {
    EqualAreaPrimitive primitive = EqualAreaPrimitive::sinusoidal;
    double central_meridian_rad = 0.0;

    double relative_area_tolerance = 1e-9;
    double absolute_area_tolerance_m2 = 1.0;

    double initial_geometric_tolerance_m = 8.0;
    double initial_local_area_tolerance_m2 = 1024.0;

    unsigned max_refinement_rounds = 12U;
    unsigned subdivision_max_depth = 32U;
    std::size_t subdivision_max_segments_per_edge = 1'000'000U;
};

struct RingProjectionResult final {
    std::vector<geometry::PlanarPoint> points;

    double source_signed_area_m2 = 0.0;
    double planar_signed_area_m2 = 0.0;
    double absolute_area_error_m2 = 0.0;
    double allowed_area_error_m2 = 0.0;

    unsigned refinement_rounds = 0U;
    std::size_t projected_vertices = 0U;

    RingProjectionError error = RingProjectionError::none;
    geometry::GeographicError geographic_error = geometry::GeographicError::none;
    SubdivisionError subdivision_error = SubdivisionError::none;
    geo::MathError sample_error = geo::MathError::none;
    std::size_t failed_edge = 0U;

    [[nodiscard]] bool ok() const noexcept {
        return error == RingProjectionError::none;
    }
};

[[nodiscard]] RingProjectionResult project_wgs84_linear_ring_verified(
    const geometry::LinearRing& ring,
    const RingProjectionOptions& options = {}
);

}  // namespace aeris::projection
