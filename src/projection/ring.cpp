// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/projection/ring.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace aeris::projection {
namespace {

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

[[nodiscard]] bool project_once(
    const geometry::LinearRing& ring,
    const RingProjectionOptions& options,
    const double geometric_tolerance,
    const double local_area_tolerance,
    RingProjectionResult& result
) {
    result.points.clear();

    const SubdivisionOptions subdivision_options{
        geometric_tolerance,
        local_area_tolerance,
        options.subdivision_max_depth,
        options.subdivision_max_segments_per_edge,
    };

    for (std::size_t index = 0U; index < ring.vertices.size(); ++index) {
        EdgeContext context{};
        context.start = ring.vertices[index];
        context.end = index + 1U < ring.vertices.size()
            ? ring.vertices[index + 1U]
            : geometry::GeodeticPoint{
                  ring.closing_longitude_rad,
                  ring.vertices.front().latitude_rad,
              };
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

        if (result.points.empty()) {
            result.points = std::move(edge.points);
        } else if (edge.points.size() > 1U) {
            result.points.insert(
                result.points.end(),
                edge.points.begin() + 1,
                edge.points.end()
            );
        }
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
