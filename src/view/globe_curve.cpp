// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/view/globe_curve.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace aeris::view {
namespace {

struct Sample final {
    double parameter = 0.0;
    GlobePoint point{};
};

struct EdgeContext final {
    geometry::GeodeticPoint start{};
    geometry::GeodeticPoint end{};
    const geo::Mat3* world_to_view = nullptr;
    double radius_m = 0.0;
};

struct State final {
    EdgeContext context{};
    const GlobeCurveOptions* options = nullptr;
    GlobeCurveResult result{};
    std::vector<geometry::PlanarPoint> current_part;
    std::size_t emitted_segments = 0U;
};

[[nodiscard]] bool valid_geodetic_point(
    const geometry::GeodeticPoint point
) noexcept {
    return std::isfinite(point.longitude_rad) &&
           std::isfinite(point.latitude_rad) &&
           point.latitude_rad >= -geo::kHalfPi &&
           point.latitude_rad <= geo::kHalfPi;
}

[[nodiscard]] bool valid_options(
    const GlobeCurveOptions& options,
    const geo::Mat3& world_to_view,
    const double radius_m
) noexcept {
    return std::isfinite(radius_m) && radius_m > 0.0 &&
           std::isfinite(options.geometric_tolerance_m) &&
           options.geometric_tolerance_m > 0.0 &&
           std::isfinite(options.horizon_tolerance_m) &&
           options.horizon_tolerance_m > 0.0 &&
           options.horizon_tolerance_m < radius_m &&
           options.max_subdivision_depth > 0U &&
           options.max_root_iterations > 0U &&
           options.max_segments > 0U &&
           geo::is_rotation_matrix(world_to_view);
}

[[nodiscard]] bool finite_globe_point(const GlobePoint point) noexcept {
    return std::isfinite(point.x) &&
           std::isfinite(point.y) &&
           std::isfinite(point.depth);
}

[[nodiscard]] bool sample(
    State& state,
    const double parameter,
    Sample& output
) {
    if (!std::isfinite(parameter) || parameter < 0.0 || parameter > 1.0) {
        state.result.error = GlobeCurveError::sample_failed;
        state.result.sample_error = geo::MathError::numerical_domain_error;
        return false;
    }

    const geometry::GeodeticPoint geodetic =
        geometry::interpolate_wgs84_linear_edge(
            state.context.start,
            state.context.end,
            parameter
        );
    const geo::ScalarResult beta = geo::authalic_latitude(
        geodetic.latitude_rad
    );
    if (!beta.ok()) {
        state.result.error = GlobeCurveError::sample_failed;
        state.result.sample_error = beta.error;
        return false;
    }

    const GlobeResult projected = orthographic_globe_point(
        geodetic.longitude_rad,
        beta.value,
        *state.context.world_to_view,
        state.context.radius_m
    );
    if (!projected.ok()) {
        state.result.error = GlobeCurveError::sample_failed;
        state.result.sample_error = projected.error;
        return false;
    }
    if (!finite_globe_point(projected.value)) {
        state.result.error = GlobeCurveError::sample_failed;
        state.result.sample_error = geo::MathError::numerical_domain_error;
        return false;
    }

    output.parameter = parameter;
    output.point = projected.value;
    return true;
}

[[nodiscard]] double point_to_segment_distance_3d(
    const GlobePoint point,
    const GlobePoint start,
    const GlobePoint end
) noexcept {
    const double dx = end.x - start.x;
    const double dy = end.y - start.y;
    const double dz = end.depth - start.depth;
    const double length_squared = dx * dx + dy * dy + dz * dz;

    if (!(length_squared > 0.0) || !std::isfinite(length_squared)) {
        return std::hypot(
            std::hypot(point.x - start.x, point.y - start.y),
            point.depth - start.depth
        );
    }

    const double projection =
        ((point.x - start.x) * dx +
         (point.y - start.y) * dy +
         (point.depth - start.depth) * dz) /
        length_squared;
    const double clamped = std::clamp(projection, 0.0, 1.0);
    const double closest_x = start.x + clamped * dx;
    const double closest_y = start.y + clamped * dy;
    const double closest_z = start.depth + clamped * dz;

    return std::hypot(
        std::hypot(point.x - closest_x, point.y - closest_y),
        point.depth - closest_z
    );
}

[[nodiscard]] bool visible(const GlobePoint point) noexcept {
    return point.depth >= 0.0;
}

[[nodiscard]] std::size_t visibility_transitions(
    const std::array<Sample, 5>& samples
) noexcept {
    std::size_t transitions = 0U;
    bool previous = visible(samples.front().point);
    for (std::size_t index = 1U; index < samples.size(); ++index) {
        const bool current = visible(samples[index].point);
        if (current != previous) {
            ++transitions;
        }
        previous = current;
    }
    return transitions;
}

[[nodiscard]] bool same_screen_point(
    const geometry::PlanarPoint left,
    const geometry::PlanarPoint right,
    const double radius_m
) noexcept {
    const double scale = std::max({
        1.0,
        radius_m,
        std::abs(left.x),
        std::abs(left.y),
        std::abs(right.x),
        std::abs(right.y),
    });
    const double tolerance =
        64.0 * std::numeric_limits<double>::epsilon() * scale;
    return std::hypot(left.x - right.x, left.y - right.y) <= tolerance;
}

[[nodiscard]] bool append_point(
    State& state,
    const GlobePoint point
) {
    const geometry::PlanarPoint planar{point.x, point.y};

    if (!state.current_part.empty() &&
        same_screen_point(
            state.current_part.back(),
            planar,
            state.context.radius_m
        )) {
        return true;
    }

    if (!state.current_part.empty()) {
        if (state.emitted_segments >= state.options->max_segments) {
            state.result.error = GlobeCurveError::limit_exceeded;
            return false;
        }
        ++state.emitted_segments;
    }

    state.current_part.push_back(planar);
    return true;
}

void finish_part(State& state) {
    if (state.current_part.size() >= 2U) {
        state.result.visible_parts.push_back(std::move(state.current_part));
        state.current_part = {};
    } else {
        state.current_part.clear();
    }
}

[[nodiscard]] bool solve_horizon(
    State& state,
    Sample left,
    Sample right,
    Sample& root
) {
    if (left.point.depth == 0.0) {
        root = left;
        return true;
    }
    if (right.point.depth == 0.0) {
        root = right;
        return true;
    }
    if (visible(left.point) == visible(right.point)) {
        state.result.error = GlobeCurveError::horizon_non_convergence;
        return false;
    }

    for (unsigned iteration = 0U;
         iteration < state.options->max_root_iterations;
         ++iteration) {
        const double midpoint_parameter =
            left.parameter + (right.parameter - left.parameter) * 0.5;
        if (!(midpoint_parameter > left.parameter) ||
            !(midpoint_parameter < right.parameter)) {
            const Sample& best =
                std::abs(left.point.depth) <= std::abs(right.point.depth)
                    ? left
                    : right;
            if (std::abs(best.point.depth) <=
                state.options->horizon_tolerance_m) {
                root = best;
                root.point.depth = 0.0;
                return true;
            }
            state.result.error = GlobeCurveError::horizon_non_convergence;
            return false;
        }

        Sample midpoint{};
        if (!sample(state, midpoint_parameter, midpoint)) {
            return false;
        }

        if (std::abs(midpoint.point.depth) <=
            state.options->horizon_tolerance_m) {
            root = midpoint;
            root.point.depth = 0.0;
            return true;
        }

        if (visible(midpoint.point) == visible(left.point)) {
            left = midpoint;
        } else {
            right = midpoint;
        }
    }

    const Sample& best =
        std::abs(left.point.depth) <= std::abs(right.point.depth)
            ? left
            : right;
    if (std::abs(best.point.depth) <= state.options->horizon_tolerance_m) {
        root = best;
        root.point.depth = 0.0;
        return true;
    }

    state.result.error = GlobeCurveError::horizon_non_convergence;
    return false;
}

[[nodiscard]] bool emit_accepted_interval(
    State& state,
    const Sample& start,
    const Sample& end
) {
    const bool start_visible = visible(start.point);
    const bool end_visible = visible(end.point);

    if (start_visible && end_visible) {
        return append_point(state, start.point) &&
               append_point(state, end.point);
    }

    if (!start_visible && !end_visible) {
        finish_part(state);
        return true;
    }

    Sample horizon{};
    if (!solve_horizon(state, start, end, horizon)) {
        return false;
    }
    ++state.result.horizon_crossings;

    if (start_visible) {
        if (!append_point(state, start.point) ||
            !append_point(state, horizon.point)) {
            return false;
        }
        finish_part(state);
        return true;
    }

    finish_part(state);
    return append_point(state, horizon.point) &&
           append_point(state, end.point);
}

[[nodiscard]] bool recurse(
    State& state,
    const Sample& start,
    const Sample& end,
    const unsigned depth
) {
    state.result.deepest_subdivision_level = std::max(
        state.result.deepest_subdivision_level,
        depth
    );

    const double span = end.parameter - start.parameter;
    const double t_quarter = start.parameter + span * 0.25;
    const double t_midpoint = start.parameter + span * 0.5;
    const double t_three_quarter = start.parameter + span * 0.75;

    if (!(t_midpoint > start.parameter) ||
        !(t_midpoint < end.parameter)) {
        state.result.error = GlobeCurveError::limit_exceeded;
        return false;
    }

    std::array<Sample, 5> samples{};
    samples[0] = start;
    samples[4] = end;
    if (!sample(state, t_quarter, samples[1]) ||
        !sample(state, t_midpoint, samples[2]) ||
        !sample(state, t_three_quarter, samples[3])) {
        return false;
    }

    const double geometric_error = std::max({
        point_to_segment_distance_3d(
            samples[1].point,
            start.point,
            end.point
        ),
        point_to_segment_distance_3d(
            samples[2].point,
            start.point,
            end.point
        ),
        point_to_segment_distance_3d(
            samples[3].point,
            start.point,
            end.point
        ),
    });
    if (!std::isfinite(geometric_error)) {
        state.result.error = GlobeCurveError::sample_failed;
        state.result.sample_error = geo::MathError::numerical_domain_error;
        return false;
    }

    const std::size_t transitions = visibility_transitions(samples);
    const bool endpoints_differ =
        visible(start.point) != visible(end.point);
    const bool topology_resolved = endpoints_differ
        ? transitions == 1U
        : transitions == 0U;

    if (geometric_error <= state.options->geometric_tolerance_m &&
        topology_resolved) {
        return emit_accepted_interval(state, start, end);
    }

    if (depth >= state.options->max_subdivision_depth) {
        state.result.error = GlobeCurveError::limit_exceeded;
        return false;
    }

    return recurse(
               state,
               start,
               samples[2],
               depth + 1U
           ) &&
           recurse(
               state,
               samples[2],
               end,
               depth + 1U
           );
}

[[nodiscard]] std::size_t polyline_segments(
    const std::vector<geometry::PlanarPoint>& part
) noexcept {
    return part.size() > 1U ? part.size() - 1U : 0U;
}

void append_ring_part(
    GlobeCurveResult& destination,
    std::vector<geometry::PlanarPoint>&& part,
    const double radius_m
) {
    if (part.size() < 2U) {
        return;
    }

    if (!destination.visible_parts.empty() &&
        !destination.visible_parts.back().empty() &&
        same_screen_point(
            destination.visible_parts.back().back(),
            part.front(),
            radius_m
        )) {
        destination.visible_parts.back().insert(
            destination.visible_parts.back().end(),
            part.begin() + 1,
            part.end()
        );
        return;
    }

    destination.visible_parts.push_back(std::move(part));
}

void merge_cyclic_ring_parts(
    GlobeCurveResult& result,
    const double radius_m
) {
    if (result.visible_parts.size() < 2U ||
        result.visible_parts.front().empty() ||
        result.visible_parts.back().empty() ||
        !same_screen_point(
            result.visible_parts.back().back(),
            result.visible_parts.front().front(),
            radius_m
        )) {
        return;
    }

    std::vector<geometry::PlanarPoint> merged =
        std::move(result.visible_parts.back());
    result.visible_parts.pop_back();
    merged.insert(
        merged.end(),
        result.visible_parts.front().begin() + 1,
        result.visible_parts.front().end()
    );
    result.visible_parts.front() = std::move(merged);
}

void update_vertex_count(GlobeCurveResult& result) noexcept {
    result.projected_vertices = 0U;
    for (const auto& part : result.visible_parts) {
        result.projected_vertices += part.size();
    }
}

}  // namespace

GlobeCurveResult project_visible_wgs84_linear_edge(
    const geometry::GeodeticPoint start,
    const geometry::GeodeticPoint end,
    const geo::Mat3& world_to_view,
    const GlobeCurveOptions& options,
    const double radius_m
) {
    GlobeCurveResult result{};

    if (!valid_options(options, world_to_view, radius_m)) {
        result.error = GlobeCurveError::invalid_options;
        return result;
    }
    if (!valid_geodetic_point(start) || !valid_geodetic_point(end)) {
        result.error = GlobeCurveError::invalid_geometry;
        return result;
    }

    if (start.longitude_rad == end.longitude_rad &&
        start.latitude_rad == end.latitude_rad) {
        return result;
    }

    State state{};
    state.context.start = start;
    state.context.end = end;
    state.context.world_to_view = &world_to_view;
    state.context.radius_m = radius_m;
    state.options = &options;

    Sample first{};
    Sample last{};
    if (!sample(state, 0.0, first) ||
        !sample(state, 1.0, last)) {
        return std::move(state.result);
    }

    static_cast<void>(recurse(state, first, last, 0U));
    if (state.result.ok()) {
        finish_part(state);
        update_vertex_count(state.result);
    }
    return std::move(state.result);
}

GlobeCurveResult project_visible_wgs84_linear_ring(
    const geometry::LinearRing& ring,
    const geo::Mat3& world_to_view,
    const GlobeCurveOptions& options,
    const double radius_m
) {
    GlobeCurveResult result{};

    if (!valid_options(options, world_to_view, radius_m)) {
        result.error = GlobeCurveError::invalid_options;
        return result;
    }
    if (ring.vertices.size() < 3U ||
        !std::isfinite(ring.closing_longitude_rad)) {
        result.error = GlobeCurveError::invalid_geometry;
        return result;
    }
    for (const geometry::GeodeticPoint point : ring.vertices) {
        if (!valid_geodetic_point(point)) {
            result.error = GlobeCurveError::invalid_geometry;
            return result;
        }
    }

    std::size_t total_segments = 0U;
    for (std::size_t edge_index = 0U;
         edge_index < ring.vertices.size();
         ++edge_index) {
        const geometry::GeodeticPoint start = ring.vertices[edge_index];
        const geometry::GeodeticPoint end =
            edge_index + 1U < ring.vertices.size()
                ? ring.vertices[edge_index + 1U]
                : geometry::GeodeticPoint{
                      ring.closing_longitude_rad,
                      ring.vertices.front().latitude_rad,
                  };

        GlobeCurveOptions edge_options = options;
        const std::size_t remaining =
            total_segments < options.max_segments
                ? options.max_segments - total_segments
                : 0U;
        edge_options.max_segments = std::max<std::size_t>(1U, remaining);

        GlobeCurveResult edge = project_visible_wgs84_linear_edge(
            start,
            end,
            world_to_view,
            edge_options,
            radius_m
        );
        if (!edge.ok()) {
            result.error = edge.error;
            result.sample_error = edge.sample_error;
            result.deepest_subdivision_level = std::max(
                result.deepest_subdivision_level,
                edge.deepest_subdivision_level
            );
            return result;
        }

        result.horizon_crossings += edge.horizon_crossings;
        result.deepest_subdivision_level = std::max(
            result.deepest_subdivision_level,
            edge.deepest_subdivision_level
        );

        for (auto& part : edge.visible_parts) {
            total_segments += polyline_segments(part);
            if (total_segments > options.max_segments) {
                result.error = GlobeCurveError::limit_exceeded;
                return result;
            }
            append_ring_part(result, std::move(part), radius_m);
        }
    }

    merge_cyclic_ring_parts(result, radius_m);
    update_vertex_count(result);
    return result;
}

}  // namespace aeris::view
