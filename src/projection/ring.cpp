// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/projection/ring.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace aeris::projection {
namespace {

constexpr double kTwoPi = 2.0 * geo::kPi;
constexpr double kSeamTolerance =
    512.0 * std::numeric_limits<double>::epsilon() * geo::kPi;

struct EdgeContext final {
    geometry::GeodeticPoint start{};
    geometry::GeodeticPoint end{};
    const ProjectionAdapter* adapter = nullptr;
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
    if (context->adapter == nullptr) {
        return {{}, geo::MathError::numerical_domain_error};
    }
    const geometry::GeodeticPoint point = geometry::interpolate_wgs84_linear_edge(
        context->start,
        context->end,
        parameter
    );

    return context->adapter->forward_wgs84(
        point.longitude_rad,
        point.latitude_rad,
        context->central_meridian_rad
    );
}

struct SeamContext final {
    double longitude_rad = 0.0;
    double start_latitude_rad = 0.0;
    double end_latitude_rad = 0.0;
    const ProjectionAdapter* adapter = nullptr;
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
    if (context->adapter == nullptr) {
        return {{}, geo::MathError::numerical_domain_error};
    }
    const double latitude =
        context->start_latitude_rad +
        parameter * (context->end_latitude_rad - context->start_latitude_rad);

    return context->adapter->forward_wgs84(
        context->longitude_rad,
        latitude,
        context->central_meridian_rad
    );
}

[[nodiscard]] bool compatible_adapter(const ProjectionAdapter* const adapter) noexcept {
    if (adapter == nullptr) return false;
    const ProjectionDescriptor descriptor = adapter->descriptor();
    return !descriptor.model_id.empty() &&
           !descriptor.display_name.empty() &&
           descriptor.cut_model_id == kProjectionCutSingleAntimeridianV1 &&
           descriptor.area_contract == ProjectionAreaContract::equal_area &&
           descriptor.cut_topology == ProjectionCutTopology::single_antimeridian;
}

[[nodiscard]] bool valid_options(const RingProjectionOptions& options) noexcept {
    return compatible_adapter(options.adapter) &&
           std::isfinite(options.central_meridian_rad) &&
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

[[nodiscard]] geometry::GeodeticPoint ring_edge_end(
    const geometry::LinearRing& ring,
    const std::size_t edge_index
) noexcept {
    if (edge_index + 1U < ring.vertices.size()) {
        return ring.vertices[edge_index + 1U];
    }
    return {
        ring.closing_longitude_rad,
        ring.vertices.front().latitude_rad,
    };
}

struct SeamIntersection final {
    std::size_t edge_index = 0U;
    double parameter = 0.0;
    double unwrapped_longitude_rad = 0.0;
    double latitude_rad = 0.0;
    double direction = 0.0;
};

[[nodiscard]] bool find_single_seam_intersection(
    const geometry::LinearRing& ring,
    const double central_meridian_rad,
    SeamIntersection& intersection
) noexcept {
    if (ring.vertices.empty()) {
        return false;
    }

    const double base_seam = central_meridian_rad + geo::kPi;
    std::size_t count = 0U;
    constexpr double parameter_tolerance =
        64.0 * std::numeric_limits<double>::epsilon();

    for (std::size_t edge_index = 0U; edge_index < ring.vertices.size(); ++edge_index) {
        const geometry::GeodeticPoint start = ring.vertices[edge_index];
        const geometry::GeodeticPoint end = ring_edge_end(ring, edge_index);
        const double delta = end.longitude_rad - start.longitude_rad;
        if (!std::isfinite(delta) || delta == 0.0) {
            continue;
        }

        const double low = std::min(start.longitude_rad, end.longitude_rad);
        const double high = std::max(start.longitude_rad, end.longitude_rad);
        const double first_k_double = std::ceil((low - base_seam) / kTwoPi) - 1.0;
        const double last_k_double = std::floor((high - base_seam) / kTwoPi) + 1.0;
        if (!std::isfinite(first_k_double) || !std::isfinite(last_k_double) ||
            first_k_double < static_cast<double>(std::numeric_limits<long long>::min()) ||
            last_k_double > static_cast<double>(std::numeric_limits<long long>::max())) {
            return false;
        }

        const long long first_k = static_cast<long long>(first_k_double);
        const long long last_k = static_cast<long long>(last_k_double);
        for (long long k = first_k; k <= last_k; ++k) {
            const double seam = base_seam + static_cast<double>(k) * kTwoPi;
            const double parameter = (seam - start.longitude_rad) / delta;

            // Half-open ownership (0, 1] means an exact seam vertex belongs to
            // its incoming edge only. This prevents double-counting the same
            // topological crossing on adjacent edges.
            if (parameter <= parameter_tolerance ||
                parameter > 1.0 + parameter_tolerance) {
                continue;
            }

            const double owned_parameter = std::min(1.0, parameter);
            const geometry::GeodeticPoint point =
                geometry::interpolate_wgs84_linear_edge(
                    start,
                    end,
                    owned_parameter
                );
            if (!std::isfinite(point.latitude_rad)) {
                return false;
            }

            ++count;
            if (count != 1U) {
                return false;
            }

            intersection.edge_index = edge_index;
            intersection.parameter = owned_parameter;
            intersection.unwrapped_longitude_rad = seam;
            intersection.latitude_rad = point.latitude_rad;
            intersection.direction = delta;
        }
    }

    return count == 1U;
}

struct ProjectionBranch final {
    bool needs_polar_closure = false;
    std::vector<geometry::GeodeticPoint> coastline_vertices;
    double start_seam_longitude_rad = 0.0;
    double end_seam_longitude_rad = 0.0;
    double seam_latitude_rad = 0.0;
    double pole_latitude_rad = 0.0;
};

[[nodiscard]] bool point_inside_projection_domain(
    const geometry::GeodeticPoint point,
    const double central_meridian_rad
) noexcept {
    const double delta = point.longitude_rad - central_meridian_rad;
    return std::isfinite(point.longitude_rad) &&
           std::isfinite(point.latitude_rad) &&
           delta >= -geo::kPi - kSeamTolerance &&
           delta <= geo::kPi + kSeamTolerance;
}

void append_shifted_point(
    std::vector<geometry::GeodeticPoint>& destination,
    const geometry::GeodeticPoint point,
    const double longitude_shift
) {
    destination.push_back({
        point.longitude_rad + longitude_shift,
        point.latitude_rad,
    });
}

[[nodiscard]] bool build_polar_projection_branch(
    const geometry::LinearRing& ring,
    const RingProjectionOptions& options,
    ProjectionBranch& branch
) {
    if (std::abs(ring.longitude_winding) != 1 ||
        ring.interior_side == geometry::RingInteriorSide::unspecified ||
        ring.vertices.empty()) {
        return false;
    }

    SeamIntersection crossing{};
    if (!find_single_seam_intersection(
            ring,
            options.central_meridian_rad,
            crossing
        )) {
        return false;
    }

    const double winding_sign = ring.longitude_winding > 0 ? 1.0 : -1.0;
    if (crossing.direction * winding_sign <= 0.0) {
        return false;
    }

    const double start_seam =
        options.central_meridian_rad - winding_sign * geo::kPi;
    const double end_seam =
        options.central_meridian_rad + winding_sign * geo::kPi;
    const double shift_after = start_seam - crossing.unwrapped_longitude_rad;
    const double shift_before = end_seam - crossing.unwrapped_longitude_rad;

    branch.coastline_vertices.clear();
    branch.coastline_vertices.reserve(ring.vertices.size() + 2U);
    branch.coastline_vertices.push_back({start_seam, crossing.latitude_rad});

    const std::size_t crossing_edge = crossing.edge_index;
    const std::size_t vertex_count = ring.vertices.size();
    constexpr double endpoint_tolerance =
        64.0 * std::numeric_limits<double>::epsilon();

    // Continue from the seam crossing to the canonical closure point using the
    // post-crossing longitude branch. If the crossing is already exactly the
    // edge endpoint, that endpoint is the start-seam point and is not repeated.
    for (std::size_t vertex = crossing_edge + 1U; vertex <= vertex_count; ++vertex) {
        if (vertex == crossing_edge + 1U &&
            crossing.parameter >= 1.0 - endpoint_tolerance) {
            continue;
        }

        const geometry::GeodeticPoint point =
            vertex < vertex_count
                ? ring.vertices[vertex]
                : geometry::GeodeticPoint{
                      ring.closing_longitude_rad,
                      ring.vertices.front().latitude_rad,
                  };
        append_shifted_point(branch.coastline_vertices, point, shift_after);
    }

    // The canonical closure point is physically the original first vertex.
    // Resume at vertex 1 on the pre-crossing longitude branch; adding vertex 0
    // again would duplicate that physical point.
    for (std::size_t vertex = 1U; vertex <= crossing_edge; ++vertex) {
        append_shifted_point(
            branch.coastline_vertices,
            ring.vertices[vertex],
            shift_before
        );
    }

    branch.coastline_vertices.push_back({end_seam, crossing.latitude_rad});

    if (branch.coastline_vertices.size() < 3U) {
        return false;
    }
    for (const geometry::GeodeticPoint point : branch.coastline_vertices) {
        if (!point_inside_projection_domain(point, options.central_meridian_rad)) {
            return false;
        }
    }

    const double pole_sign =
        ring.interior_side == geometry::RingInteriorSide::left
            ? winding_sign
            : -winding_sign;

    branch.needs_polar_closure = true;
    branch.start_seam_longitude_rad = start_seam;
    branch.end_seam_longitude_rad = end_seam;
    branch.seam_latitude_rad = crossing.latitude_rad;
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

[[nodiscard]] bool project_edge(
    const geometry::GeodeticPoint start,
    const geometry::GeodeticPoint end,
    const RingProjectionOptions& options,
    const SubdivisionOptions& subdivision_options,
    const std::size_t failed_edge,
    RingProjectionResult& result
) {
    EdgeContext context{};
    context.start = start;
    context.end = end;
    context.adapter = options.adapter;
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
        result.failed_edge = failed_edge;
        return false;
    }

    append_curve_points(result.points, std::move(edge));
    return true;
}

[[nodiscard]] bool append_polar_seam_closure(
    const ProjectionBranch& branch,
    const RingProjectionOptions& options,
    const SubdivisionOptions& subdivision_options,
    const std::size_t first_synthetic_edge,
    RingProjectionResult& result
) {
    SeamContext to_pole{};
    to_pole.longitude_rad = branch.end_seam_longitude_rad;
    to_pole.start_latitude_rad = branch.seam_latitude_rad;
    to_pole.end_latitude_rad = branch.pole_latitude_rad;
    to_pole.adapter = options.adapter;
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
        result.failed_edge = first_synthetic_edge;
        return false;
    }
    append_curve_points(result.points, std::move(first));

    SeamContext from_pole{};
    from_pole.longitude_rad = branch.start_seam_longitude_rad;
    from_pole.start_latitude_rad = branch.pole_latitude_rad;
    from_pole.end_latitude_rad = branch.seam_latitude_rad;
    from_pole.adapter = options.adapter;
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
        result.failed_edge = first_synthetic_edge + 1U;
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

    const SubdivisionOptions subdivision_options{
        geometric_tolerance,
        local_area_tolerance,
        options.subdivision_max_depth,
        options.subdivision_max_segments_per_edge,
    };

    if (ring.longitude_winding == 0) {
        for (std::size_t index = 0U; index < ring.vertices.size(); ++index) {
            const geometry::GeodeticPoint end = ring_edge_end(ring, index);
            if (!project_edge(
                    ring.vertices[index],
                    end,
                    options,
                    subdivision_options,
                    index,
                    result
                )) {
                return false;
            }
        }
        result.projected_vertices = result.points.size();
        return true;
    }

    ProjectionBranch branch{};
    if (!build_polar_projection_branch(ring, options, branch)) {
        result.error = RingProjectionError::unsupported_seam_topology;
        return false;
    }

    for (std::size_t index = 0U;
         index + 1U < branch.coastline_vertices.size();
         ++index) {
        if (!project_edge(
                branch.coastline_vertices[index],
                branch.coastline_vertices[index + 1U],
                options,
                subdivision_options,
                index,
                result
            )) {
            return false;
        }
    }

    if (branch.needs_polar_closure &&
        !append_polar_seam_closure(
            branch,
            options,
            subdivision_options,
            branch.coastline_vertices.size() - 1U,
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
