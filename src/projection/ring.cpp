// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/projection/ring.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace aeris::projection {
namespace {

constexpr double kTwoPi = 2.0 * geo::kPi;

struct EdgeContext final {
    geometry::GeodeticPoint start{};
    geometry::GeodeticPoint end{};
    EqualAreaPrimitive primitive = EqualAreaPrimitive::sinusoidal;
    double central_meridian_rad = 0.0;
};

[[nodiscard]] PlanarResult sample_edge(
    const double parameter,
    void* const opaque_context
) noexcept {
    if (opaque_context == nullptr || !std::isfinite(parameter)) {
        return {{}, geo::MathError::non_finite_input};
    }

    const auto* const context = static_cast<const EdgeContext*>(opaque_context);
    const geometry::GeodeticPoint point = geometry::interpolate_wgs84_linear_edge(
        context->start,
        context->end,
        parameter
    );

    return project_wgs84_primitive(
        point.longitude_rad,
        point.latitude_rad,
        context->primitive,
        context->central_meridian_rad
    );
}

struct SeamContext final {
    double longitude_rad = 0.0;
    double start_latitude_rad = 0.0;
    double end_latitude_rad = 0.0;
    EqualAreaPrimitive primitive = EqualAreaPrimitive::sinusoidal;
    double central_meridian_rad = 0.0;
};

[[nodiscard]] PlanarResult sample_seam(
    const double parameter,
    void* const opaque_context
) noexcept {
    if (opaque_context == nullptr || !std::isfinite(parameter)) {
        return {{}, geo::MathError::non_finite_input};
    }

    const auto* const context = static_cast<const SeamContext*>(opaque_context);
    const double latitude =
        context->start_latitude_rad +
        parameter * (context->end_latitude_rad - context->start_latitude_rad);

    return project_wgs84_primitive(
        context->longitude_rad,
        latitude,
        context->primitive,
        context->central_meridian_rad
    );
}

[[nodiscard]] bool valid_options(const RingProjectionOptions& options) noexcept {
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
           options.subdivision_max_segments_per_edge > 0U;
}

struct ProjectionBranch final {
    double longitude_shift_rad = 0.0;
    bool needs_polar_closure = false;
    double start_seam_longitude_rad = 0.0;
    double end_seam_longitude_rad = 0.0;
    double pole_latitude_rad = 0.0;
};

[[nodiscard]] bool select_projection_branch(
    const geometry::LinearRing& ring,
    const RingProjectionOptions& options,
    ProjectionBranch& branch
) noexcept {
    if (ring.longitude_winding == 0) {
        return true;
    }

    if (std::abs(ring.longitude_winding) != 1 ||
        ring.interior_side == geometry::RingInteriorSide::unspecified ||
        ring.vertices.empty()) {
        return false;
    }

    const double start_longitude = ring.vertices.front().longitude_rad;
    const double closing_longitude = ring.closing_longitude_rad;
    const double midpoint = 0.5 * (start_longitude + closing_longitude);
    if (!std::isfinite(midpoint)) {
        return false;
    }

    // A polar ring that is already cut at the map seam may be shifted only by
    // whole longitude turns. If its closure is elsewhere, a general seam
    // splitter must cut/reorder it first; this routine deliberately refuses to
    // invent that topology.
    const double turns = std::round(
        (options.central_meridian_rad - midpoint) / kTwoPi
    );
    const double shift = turns * kTwoPi;
    const double shifted_start = start_longitude + shift;
    const double shifted_close = closing_longitude + shift;
    const double start_delta = shifted_start - options.central_meridian_rad;
    const double close_delta = shifted_close - options.central_meridian_rad;

    constexpr double seam_tolerance =
        512.0 * std::numeric_limits<double>::epsilon() * geo::kPi;
    const bool endpoints_on_opposite_seams =
        std::abs(std::abs(start_delta) - geo::kPi) <= seam_tolerance &&
        std::abs(std::abs(close_delta) - geo::kPi) <= seam_tolerance &&
        start_delta * close_delta < 0.0;
    if (!endpoints_on_opposite_seams) {
        return false;
    }

    for (const geometry::GeodeticPoint point : ring.vertices) {
        const double delta = point.longitude_rad + shift - options.central_meridian_rad;
        if (!std::isfinite(delta) ||
            delta < -geo::kPi - seam_tolerance ||
            delta > geo::kPi + seam_tolerance) {
            return false;
        }
    }

    const double winding_sign = ring.longitude_winding > 0 ? 1.0 : -1.0;
    const double pole_sign =
        ring.interior_side == geometry::RingInteriorSide::left
            ? winding_sign
            : -winding_sign;

    branch.longitude_shift_rad = shift;
    branch.needs_polar_closure = true;
    branch.start_seam_longitude_rad =
        options.central_meridian_rad + (start_delta < 0.0 ? -geo::kPi : geo::kPi);
    branch.end_seam_longitude_rad =
        options.central_meridian_rad + (close_delta < 0.0 ? -geo::kPi : geo::kPi);
    branch.pole_latitude_rad = pole_sign * geo::kHalfPi;
    return true;
}

void append_curve_points(
    std::vector<geometry::PlanarPoint>& destination,
    SubdivisionResult&& curve
) {
    if (destination.empty()) {
        destination = std::move(curve.points);
        return;
    }
    if (curve.points.size() > 1U) {
        destination.insert(
            destination.end(),
            curve.points.begin() + 1,
            curve.points.end()
        );
    }
}

[[nodiscard]] bool append_polar_seam_closure(
    const geometry::LinearRing& ring,
    const ProjectionBranch& branch,
    const RingProjectionOptions& options,
    const SubdivisionOptions& subdivision_options,
    RingProjectionResult& result
) {
    const double boundary_latitude = ring.vertices.front().latitude_rad;

    SeamContext to_pole{};
    to_pole.longitude_rad = branch.end_seam_longitude_rad;
    to_pole.start_latitude_rad = boundary_latitude;
    to_pole.end_latitude_rad = branch.pole_latitude_rad;
    to_pole.primitive = options.primitive;
    to_pole.central_meridian_rad = options.central_meridian_rad;

    SubdivisionResult first = subdivide_projected_curve(
        sample_seam,
        &to_pole,
        subdivision_options
    );
    if (!first.ok()) {
        result.error = RingProjectionError::subdivision_failed;
        result.subdivision_error = first.error;
        result.sample_error = first.sample_error;
        result.failed_edge = ring.vertices.size();
        return false;
    }
    append_curve_points(result.points, std::move(first));

    SeamContext from_pole{};
    from_pole.longitude_rad = branch.start_seam_longitude_rad;
    from_pole.start_latitude_rad = branch.pole_latitude_rad;
    from_pole.end_latitude_rad = boundary_latitude;
    from_pole.primitive = options.primitive;
    from_pole.central_meridian_rad = options.central_meridian_rad;

    SubdivisionResult second = subdivide_projected_curve(
        sample_seam,
        &from_pole,
        subdivision_options
    );
    if (!second.ok()) {
        result.error = RingProjectionError::subdivision_failed;
        result.subdivision_error = second.error;
        result.sample_error = second.sample_error;
        result.failed_edge = ring.vertices.size() + 1U;
        return false;
    }
    append_curve_points(result.points, std::move(second));
    return true;
}

[[nodiscard]] bool project_once(
    const geometry::LinearRing& ring,
    const RingProjectionOptions& options,
    const double geometric_tolerance,
    const double local_area_tolerance,
    RingProjectionResult& result
) {
    result.points.clear();

    ProjectionBranch branch{};
    if (!select_projection_branch(ring, options, branch)) {
        result.error = RingProjectionError::unsupported_seam_topology;
        return false;
    }

    const SubdivisionOptions subdivision_options{
        geometric_tolerance,
        local_area_tolerance,
        options.subdivision_max_depth,
        options.subdivision_max_segments_per_edge,
    };

    for (std::size_t index = 0U; index < ring.vertices.size(); ++index) {
        EdgeContext context{};
        context.start = ring.vertices[index];
        context.start.longitude_rad += branch.longitude_shift_rad;
        context.end = index + 1U < ring.vertices.size()
            ? ring.vertices[index + 1U]
            : geometry::GeodeticPoint{
                  ring.closing_longitude_rad,
                  ring.vertices.front().latitude_rad,
              };
        context.end.longitude_rad += branch.longitude_shift_rad;
        context.primitive = options.primitive;
        context.central_meridian_rad = options.central_meridian_rad;

        SubdivisionResult edge = subdivide_projected_curve(
            sample_edge,
            &context,
            subdivision_options
        );
        if (!edge.ok()) {
            result.error = RingProjectionError::subdivision_failed;
            result.subdivision_error = edge.error;
            result.sample_error = edge.sample_error;
            result.failed_edge = index;
            return false;
        }

        append_curve_points(result.points, std::move(edge));
    }

    if (branch.needs_polar_closure &&
        !append_polar_seam_closure(
            ring,
            branch,
            options,
            subdivision_options,
            result
        )) {
        return false;
    }

    result.projected_vertices = result.points.size();
    return true;
}

}  // namespace

RingProjectionResult project_wgs84_linear_ring_verified(
    const geometry::LinearRing& ring,
    const RingProjectionOptions& options
) {
    RingProjectionResult result{};

    if (!valid_options(options)) {
        result.error = RingProjectionError::invalid_options;
        return result;
    }

    const geometry::GeographicAreaResult source_area =
        geometry::signed_wgs84_linear_ring_area(ring);
    if (!source_area.ok()) {
        result.error = RingProjectionError::geographic_area_failed;
        result.geographic_error = source_area.error;
        return result;
    }

    result.source_signed_area_m2 = source_area.signed_area_m2;
    result.allowed_area_error_m2 = std::max(
        options.absolute_area_tolerance_m2,
        options.relative_area_tolerance * std::abs(result.source_signed_area_m2)
    );

    if (!std::isfinite(result.allowed_area_error_m2)) {
        result.error = RingProjectionError::invalid_options;
        return result;
    }

    double geometric_tolerance = options.initial_geometric_tolerance_m;
    double local_area_tolerance = options.initial_local_area_tolerance_m2;

    for (unsigned round = 0U; round < options.max_refinement_rounds; ++round) {
        result.refinement_rounds = round + 1U;
        result.error = RingProjectionError::none;
        result.subdivision_error = SubdivisionError::none;
        result.sample_error = geo::MathError::none;
        result.failed_edge = 0U;

        if (!project_once(
                ring,
                options,
                geometric_tolerance,
                local_area_tolerance,
                result
            )) {
            return result;
        }

        result.planar_signed_area_m2 = geometry::signed_planar_area(result.points);
        if (!std::isfinite(result.planar_signed_area_m2)) {
            result.error = RingProjectionError::non_finite_planar_area;
            return result;
        }

        result.absolute_area_error_m2 = std::abs(
            result.planar_signed_area_m2 - result.source_signed_area_m2
        );
        if (!std::isfinite(result.absolute_area_error_m2)) {
            result.error = RingProjectionError::non_finite_planar_area;
            return result;
        }

        if (result.absolute_area_error_m2 <= result.allowed_area_error_m2) {
            return result;
        }

        geometric_tolerance *= 0.5;
        local_area_tolerance *= 0.25;
        if (!std::isfinite(geometric_tolerance) || geometric_tolerance <= 0.0 ||
            !std::isfinite(local_area_tolerance) || local_area_tolerance <= 0.0) {
            break;
        }
    }

    result.error = RingProjectionError::area_budget_unmet;
    return result;
}

}  // namespace aeris::projection
