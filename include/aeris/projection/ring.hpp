// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "aeris/geometry/geographic.hpp"
#include "aeris/geometry/planar.hpp"
#include "aeris/projection/adapter.hpp"
#include "aeris/projection/seam.hpp"
#include "aeris/projection/subdivide.hpp"

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
    // Non-owning adapter reference. Built-in adapters have static lifetime;
    // callers supplying custom adapters must keep them alive for the projection
    // call. The verified pipeline accepts only adapters declaring the AERIS
    // equal-area + single-antimeridian contracts it can currently prove.
    const ProjectionAdapter* adapter = &sinusoidal_projection_adapter();
    double central_meridian_rad = 0.0;

    double relative_area_tolerance = 1e-9;
    double absolute_area_tolerance_m2 = 1.0;

    double initial_geometric_tolerance_m = 8.0;
    double initial_local_area_tolerance_m2 = 1024.0;

    unsigned max_refinement_rounds = 12U;
    unsigned subdivision_max_depth = 32U;
    std::size_t subdivision_max_segments_per_edge = 1'000'000U;
    std::size_t max_projection_pieces = 4096U;
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

enum class PiecewiseRingProjectionError {
    none = 0,
    invalid_options,
    geographic_area_failed,
    seam_split_failed,
    piece_projection_failed,
    piece_orientation_changed,
    non_finite_planar_area,
    aggregate_area_budget_unmet,
};

struct PiecewiseRingProjectionResult final {
    std::vector<std::vector<geometry::PlanarPoint>> pieces;

    double source_signed_area_m2 = 0.0;
    double planar_signed_area_m2 = 0.0;
    double absolute_area_error_m2 = 0.0;
    double allowed_area_error_m2 = 0.0;
    double seam_partition_error_m2 = 0.0;

    std::size_t seam_crossings = 0U;
    std::size_t projected_pieces = 0U;
    std::size_t projected_vertices = 0U;
    unsigned max_piece_refinement_rounds = 0U;

    PiecewiseRingProjectionError error = PiecewiseRingProjectionError::none;
    SeamSplitError seam_error = SeamSplitError::none;
    RingProjectionError piece_error = RingProjectionError::none;
    geometry::GeographicError geographic_error = geometry::GeographicError::none;
    SubdivisionError subdivision_error = SubdivisionError::none;
    geo::MathError sample_error = geo::MathError::none;
    std::size_t failed_piece = 0U;
    std::size_t failed_edge = 0U;

    [[nodiscard]] bool ok() const noexcept {
        return error == PiecewiseRingProjectionError::none;
    }
};

// Verified high-level ring projection. Zero-winding rings are first partitioned
// at the active projection seam when necessary; every resulting geographic
// piece then passes through the adapter-backed single-ring verifier. Polar
// nonzero-winding rings remain owned by the dedicated polar seam contract.
// The final signed planar sum is checked again against the original WGS84 ring
// under one global area budget.
[[nodiscard]] PiecewiseRingProjectionResult project_wgs84_linear_ring_piecewise_verified(
    const geometry::LinearRing& ring,
    const RingProjectionOptions& options = {}
);

}  // namespace aeris::projection
