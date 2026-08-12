// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/projection/subdivide.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace aeris::projection {
namespace {

[[nodiscard]] bool finite_point(const PlanarPoint point) noexcept {
    return std::isfinite(point.x) && std::isfinite(point.y);
}

[[nodiscard]] double point_to_segment_distance(
    const PlanarPoint point,
    const PlanarPoint start,
    const PlanarPoint end
) noexcept {
    const double dx = end.x - start.x;
    const double dy = end.y - start.y;
    const double length_squared = dx * dx + dy * dy;

    if (!(length_squared > 0.0) || !std::isfinite(length_squared)) {
        return std::hypot(point.x - start.x, point.y - start.y);
    }

    const double projection =
        ((point.x - start.x) * dx + (point.y - start.y) * dy) / length_squared;
    const double clamped = std::max(0.0, std::min(1.0, projection));
    const double closest_x = start.x + clamped * dx;
    const double closest_y = start.y + clamped * dy;
    return std::hypot(point.x - closest_x, point.y - closest_y);
}

[[nodiscard]] double conservative_area_envelope(
    const std::array<PlanarPoint, 5>& samples
) noexcept {
    double area = 0.0;
    const PlanarPoint origin = samples.front();

    for (std::size_t index = 1U; index + 1U < samples.size(); ++index) {
        const double ax = samples[index].x - origin.x;
        const double ay = samples[index].y - origin.y;
        const double bx = samples[index + 1U].x - origin.x;
        const double by = samples[index + 1U].y - origin.y;
        const double triangle = 0.5 * std::abs(ax * by - ay * bx);
        area += triangle;
    }

    return area;
}

struct State final {
    ProjectedCurveSampler sampler = nullptr;
    void* context = nullptr;
    const SubdivisionOptions* options = nullptr;
    SubdivisionResult result{};
};

[[nodiscard]] bool sample(
    State& state,
    const double parameter,
    PlanarPoint& output
) {
    const PlanarResult sampled = state.sampler(parameter, state.context);
    if (!sampled.ok()) {
        state.result.error = SubdivisionError::sample_failed;
        state.result.sample_error = sampled.error;
        return false;
    }
    if (!finite_point(sampled.value)) {
        state.result.error = SubdivisionError::non_finite_sample;
        return false;
    }

    output = sampled.value;
    return true;
}

[[nodiscard]] bool recurse(
    State& state,
    const double t0,
    const PlanarPoint p0,
    const double t1,
    const PlanarPoint p1,
    const unsigned depth
) {
    state.result.deepest_level = std::max(state.result.deepest_level, depth);

    const double span = t1 - t0;
    const double t_quarter = t0 + span * 0.25;
    const double t_midpoint = t0 + span * 0.5;
    const double t_three_quarter = t0 + span * 0.75;

    std::array<PlanarPoint, 5> points{};
    points[0] = p0;
    points[4] = p1;
    if (!sample(state, t_quarter, points[1]) ||
        !sample(state, t_midpoint, points[2]) ||
        !sample(state, t_three_quarter, points[3])) {
        return false;
    }

    const double geometric_error = std::max({
        point_to_segment_distance(points[1], p0, p1),
        point_to_segment_distance(points[2], p0, p1),
        point_to_segment_distance(points[3], p0, p1),
    });
    const double area_error = conservative_area_envelope(points);

    if (!std::isfinite(geometric_error) || !std::isfinite(area_error)) {
        state.result.error = SubdivisionError::non_finite_sample;
        return false;
    }

    if (geometric_error <= state.options->geometric_tolerance &&
        area_error <= state.options->area_tolerance) {
        if (state.result.points.size() >= state.options->max_segments + 1U) {
            state.result.error = SubdivisionError::limit_exceeded;
            return false;
        }
        state.result.points.push_back(p1);
        return true;
    }

    if (depth >= state.options->max_depth) {
        state.result.error = SubdivisionError::limit_exceeded;
        return false;
    }

    return recurse(state, t0, p0, t_midpoint, points[2], depth + 1U) &&
           recurse(state, t_midpoint, points[2], t1, p1, depth + 1U);
}

}  // namespace

SubdivisionResult subdivide_projected_curve(
    const ProjectedCurveSampler sampler,
    void* const context,
    const SubdivisionOptions& options
) {
    SubdivisionResult result{};

    if (sampler == nullptr ||
        !std::isfinite(options.geometric_tolerance) || options.geometric_tolerance <= 0.0 ||
        !std::isfinite(options.area_tolerance) || options.area_tolerance <= 0.0 ||
        options.max_depth == 0U || options.max_segments == 0U) {
        result.error = SubdivisionError::invalid_options;
        return result;
    }

    State state{};
    state.sampler = sampler;
    state.context = context;
    state.options = &options;

    PlanarPoint start{};
    PlanarPoint end{};
    if (!sample(state, 0.0, start) || !sample(state, 1.0, end)) {
        return std::move(state.result);
    }

    state.result.points.reserve(64U);
    state.result.points.push_back(start);
    static_cast<void>(recurse(state, 0.0, start, 1.0, end, 0U));
    return std::move(state.result);
}

}  // namespace aeris::projection
