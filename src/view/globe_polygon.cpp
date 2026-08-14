// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/view/globe_polygon.hpp"

#include "aeris/geo/wgs84.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace aeris::view {
namespace {

constexpr double kTwoPi = 2.0 * geo::kPi;

struct Endpoint final {
    std::size_t chain_index = 0U;
    bool is_start = false;
    double angle_rad = 0.0;
};

enum class HemisphereCoverage {
    minor = 0,
    major,
    ambiguous,
};

[[nodiscard]] bool valid_options(
    const GlobePolygonOptions& options,
    const geo::Mat3& world_to_view,
    const double radius_m
) noexcept {
    return std::isfinite(radius_m) && radius_m > 0.0 &&
           geo::is_rotation_matrix(world_to_view) &&
           std::isfinite(options.curve.geometric_tolerance_m) &&
           options.curve.geometric_tolerance_m > 0.0 &&
           std::isfinite(options.curve.horizon_tolerance_m) &&
           options.curve.horizon_tolerance_m > 0.0 &&
           options.curve.horizon_tolerance_m < radius_m &&
           options.curve.max_subdivision_depth > 0U &&
           options.curve.max_root_iterations > 0U &&
           options.curve.max_segments > 0U &&
           std::isfinite(options.horizon_arc_tolerance_m) &&
           options.horizon_arc_tolerance_m > 0.0 &&
           options.horizon_arc_tolerance_m < radius_m &&
           options.max_horizon_arc_segments > 0U &&
           options.max_output_rings > 0U;
}

[[nodiscard]] bool valid_ring(const geometry::LinearRing& ring) noexcept {
    if (ring.vertices.size() < 3U || !std::isfinite(ring.closing_longitude_rad)) {
        return false;
    }
    for (const geometry::GeodeticPoint point : ring.vertices) {
        if (!std::isfinite(point.longitude_rad) ||
            !std::isfinite(point.latitude_rad) ||
            point.latitude_rad < -geo::kHalfPi ||
            point.latitude_rad > geo::kHalfPi) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] double point_tolerance_m(const double radius_m) noexcept {
    return 128.0 * std::numeric_limits<double>::epsilon() *
        std::max(1.0, radius_m);
}

[[nodiscard]] bool same_point(
    const geometry::PlanarPoint left,
    const geometry::PlanarPoint right,
    const double radius_m
) noexcept {
    return std::hypot(left.x - right.x, left.y - right.y) <=
        point_tolerance_m(radius_m);
}

void remove_duplicate_terminal(
    std::vector<geometry::PlanarPoint>& ring,
    const double radius_m
) {
    if (ring.size() >= 2U && same_point(ring.front(), ring.back(), radius_m)) {
        ring.pop_back();
    }
}

[[nodiscard]] double positive_angle(const double angle_rad) noexcept {
    double value = std::fmod(angle_rad, kTwoPi);
    if (value < 0.0) {
        value += kTwoPi;
    }
    return value;
}

[[nodiscard]] double limb_angle(
    const geometry::PlanarPoint point
) noexcept {
    return positive_angle(std::atan2(point.y, point.x));
}

[[nodiscard]] double angular_tolerance(
    const GlobePolygonOptions& options,
    const double radius_m
) noexcept {
    return std::max(
        1024.0 * std::numeric_limits<double>::epsilon() * geo::kPi,
        8.0 * options.curve.horizon_tolerance_m / radius_m
    );
}

[[nodiscard]] bool point_on_limb(
    const geometry::PlanarPoint point,
    const GlobePolygonOptions& options,
    const double radius_m
) noexcept {
    const double radial = std::hypot(point.x, point.y);
    const double tolerance = std::max(
        1.0,
        8.0 * options.curve.horizon_tolerance_m
    );
    return std::isfinite(radial) &&
           std::abs(radial - radius_m) <= tolerance;
}

[[nodiscard]] HemisphereCoverage classify_coverage(
    const geometry::GeographicAreaResult& area,
    const double radius_m
) noexcept {
    const double half_surface_area = 2.0 * geo::kPi * radius_m * radius_m;
    const double magnitude = std::abs(area.signed_area_m2);
    const double numerical_floor =
        64.0 * std::numeric_limits<double>::epsilon() *
        std::max({1.0, magnitude, half_surface_area});
    const double uncertainty = area.estimated_abs_error_m2 + numerical_floor;

    if (!std::isfinite(half_surface_area) ||
        !std::isfinite(magnitude) ||
        !std::isfinite(uncertainty)) {
        return HemisphereCoverage::ambiguous;
    }
    if (magnitude < half_surface_area - uncertainty) {
        return HemisphereCoverage::minor;
    }
    if (magnitude > half_surface_area + uncertainty) {
        return HemisphereCoverage::major;
    }
    return HemisphereCoverage::ambiguous;
}

[[nodiscard]] double maximum_arc_step(
    const GlobePolygonOptions& options,
    const double radius_m
) noexcept {
    const double cosine = std::clamp(
        1.0 - options.horizon_arc_tolerance_m / radius_m,
        -1.0,
        1.0
    );
    return 2.0 * std::acos(cosine);
}

[[nodiscard]] bool reserve_arc_segments(
    const double delta_angle,
    const GlobePolygonOptions& options,
    const double radius_m,
    std::size_t& segment_count,
    GlobePolygonResult& result
) noexcept {
    const double step = maximum_arc_step(options, radius_m);
    if (!std::isfinite(step) || step <= 0.0 ||
        !std::isfinite(delta_angle) || delta_angle == 0.0) {
        result.error = GlobePolygonError::ambiguous_horizon_topology;
        return false;
    }

    const double raw_count = std::ceil(std::abs(delta_angle) / step);
    if (!std::isfinite(raw_count) ||
        raw_count > static_cast<double>(std::numeric_limits<std::size_t>::max())) {
        result.error = GlobePolygonError::horizon_arc_limit_exceeded;
        return false;
    }

    segment_count = std::max<std::size_t>(
        1U,
        static_cast<std::size_t>(raw_count)
    );
    if (segment_count > options.max_horizon_arc_segments -
            std::min(options.max_horizon_arc_segments, result.horizon_arc_segments)) {
        result.error = GlobePolygonError::horizon_arc_limit_exceeded;
        return false;
    }

    result.horizon_arc_segments += segment_count;
    return true;
}

[[nodiscard]] bool append_limb_arc(
    std::vector<geometry::PlanarPoint>& destination,
    const double start_angle,
    const double end_angle,
    const geometry::RingInteriorSide interior_side,
    const GlobePolygonOptions& options,
    const double radius_m,
    GlobePolygonResult& result
) {
    double delta = 0.0;
    if (interior_side == geometry::RingInteriorSide::left) {
        delta = positive_angle(end_angle - start_angle);
    } else if (interior_side == geometry::RingInteriorSide::right) {
        delta = -positive_angle(start_angle - end_angle);
    } else {
        result.error = GlobePolygonError::missing_interior_side;
        return false;
    }

    if (std::abs(delta) <= angular_tolerance(options, radius_m)) {
        result.error = GlobePolygonError::ambiguous_horizon_topology;
        return false;
    }

    std::size_t segments = 0U;
    if (!reserve_arc_segments(delta, options, radius_m, segments, result)) {
        return false;
    }

    for (std::size_t index = 1U; index <= segments; ++index) {
        const double parameter =
            static_cast<double>(index) / static_cast<double>(segments);
        const double angle = start_angle + parameter * delta;
        destination.push_back({
            radius_m * std::cos(angle),
            radius_m * std::sin(angle),
        });
    }
    return true;
}

[[nodiscard]] bool append_full_limb(
    const geometry::RingInteriorSide interior_side,
    const GlobePolygonOptions& options,
    const double radius_m,
    GlobePolygonResult& result
) {
    if (result.rings.size() >= options.max_output_rings) {
        result.error = GlobePolygonError::output_ring_limit_exceeded;
        return false;
    }

    const double delta =
        interior_side == geometry::RingInteriorSide::left
            ? kTwoPi
            : -kTwoPi;
    if (interior_side == geometry::RingInteriorSide::unspecified) {
        result.error = GlobePolygonError::missing_interior_side;
        return false;
    }

    std::size_t segments = 0U;
    if (!reserve_arc_segments(delta, options, radius_m, segments, result)) {
        return false;
    }
    segments = std::max<std::size_t>(segments, 4U);
    if (segments > options.max_horizon_arc_segments ||
        segments > result.horizon_arc_segments +
            (options.max_horizon_arc_segments - result.horizon_arc_segments)) {
        result.error = GlobePolygonError::horizon_arc_limit_exceeded;
        return false;
    }

    // reserve_arc_segments already accounted for its original count. If the
    // four-vertex minimum increased it, account for the additional segments.
    const std::size_t already_accounted = result.horizon_arc_segments;
    const double raw_count = std::ceil(
        std::abs(delta) / maximum_arc_step(options, radius_m)
    );
    const std::size_t original = std::max<std::size_t>(
        1U,
        static_cast<std::size_t>(raw_count)
    );
    if (segments > original) {
        const std::size_t extra = segments - original;
        if (extra > options.max_horizon_arc_segments - already_accounted) {
            result.error = GlobePolygonError::horizon_arc_limit_exceeded;
            return false;
        }
        result.horizon_arc_segments += extra;
    }

    std::vector<geometry::PlanarPoint> limb;
    limb.reserve(segments);
    for (std::size_t index = 0U; index < segments; ++index) {
        const double parameter =
            static_cast<double>(index) / static_cast<double>(segments);
        const double angle = parameter * delta;
        limb.push_back({
            radius_m * std::cos(angle),
            radius_m * std::sin(angle),
        });
    }
    result.rings.push_back(std::move(limb));
    return true;
}

[[nodiscard]] bool add_closed_source_boundary(
    std::vector<geometry::PlanarPoint> boundary,
    const GlobePolygonOptions& options,
    const double radius_m,
    GlobePolygonResult& result
) {
    remove_duplicate_terminal(boundary, radius_m);
    if (boundary.size() < 3U) {
        result.error = GlobePolygonError::ambiguous_horizon_topology;
        return false;
    }
    if (result.rings.size() >= options.max_output_rings) {
        result.error = GlobePolygonError::output_ring_limit_exceeded;
        return false;
    }
    result.rings.push_back(std::move(boundary));
    return true;
}

[[nodiscard]] bool validate_endpoint_order(
    const std::vector<Endpoint>& endpoints,
    const geometry::RingInteriorSide interior_side,
    const double tolerance,
    std::vector<std::size_t>& next_chain,
    GlobePolygonResult& result
) {
    if (endpoints.empty() || (endpoints.size() % 2U) != 0U) {
        result.error = GlobePolygonError::ambiguous_horizon_topology;
        return false;
    }

    for (std::size_t index = 1U; index < endpoints.size(); ++index) {
        if (std::abs(endpoints[index].angle_rad -
                     endpoints[index - 1U].angle_rad) <= tolerance) {
            result.error = GlobePolygonError::ambiguous_horizon_topology;
            return false;
        }
    }
    if (positive_angle(
            endpoints.front().angle_rad + kTwoPi -
            endpoints.back().angle_rad
        ) <= tolerance) {
        result.error = GlobePolygonError::ambiguous_horizon_topology;
        return false;
    }

    const std::size_t no_chain = std::numeric_limits<std::size_t>::max();
    std::vector<bool> start_used(next_chain.size(), false);

    for (std::size_t index = 0U; index < endpoints.size(); ++index) {
        const Endpoint& endpoint = endpoints[index];
        if (endpoint.is_start) {
            continue;
        }

        std::size_t target_index = 0U;
        if (interior_side == geometry::RingInteriorSide::left) {
            target_index = (index + 1U) % endpoints.size();
        } else if (interior_side == geometry::RingInteriorSide::right) {
            target_index = (index + endpoints.size() - 1U) % endpoints.size();
        } else {
            result.error = GlobePolygonError::missing_interior_side;
            return false;
        }

        const Endpoint& target = endpoints[target_index];
        if (!target.is_start ||
            endpoint.chain_index >= next_chain.size() ||
            target.chain_index >= next_chain.size() ||
            next_chain[endpoint.chain_index] != no_chain ||
            start_used[target.chain_index]) {
            result.error = GlobePolygonError::ambiguous_horizon_topology;
            return false;
        }

        next_chain[endpoint.chain_index] = target.chain_index;
        start_used[target.chain_index] = true;
    }

    for (std::size_t index = 0U; index < next_chain.size(); ++index) {
        if (next_chain[index] == no_chain || !start_used[index]) {
            result.error = GlobePolygonError::ambiguous_horizon_topology;
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool build_partial_rings(
    const GlobeCurveResult& curve,
    const geometry::RingInteriorSide interior_side,
    const GlobePolygonOptions& options,
    const double radius_m,
    GlobePolygonResult& result
) {
    const std::size_t chain_count = curve.visible_parts.size();
    if (chain_count == 0U || chain_count > options.max_output_rings) {
        result.error = chain_count == 0U
            ? GlobePolygonError::ambiguous_horizon_topology
            : GlobePolygonError::output_ring_limit_exceeded;
        return false;
    }

    std::vector<Endpoint> endpoints;
    endpoints.reserve(chain_count * 2U);

    for (std::size_t chain_index = 0U;
         chain_index < chain_count;
         ++chain_index) {
        const auto& chain = curve.visible_parts[chain_index];
        if (chain.size() < 2U ||
            same_point(chain.front(), chain.back(), radius_m) ||
            !point_on_limb(chain.front(), options, radius_m) ||
            !point_on_limb(chain.back(), options, radius_m)) {
            result.error = GlobePolygonError::ambiguous_horizon_topology;
            return false;
        }

        endpoints.push_back({
            chain_index,
            true,
            limb_angle(chain.front()),
        });
        endpoints.push_back({
            chain_index,
            false,
            limb_angle(chain.back()),
        });
    }

    std::sort(
        endpoints.begin(),
        endpoints.end(),
        [](const Endpoint& left, const Endpoint& right) {
            return left.angle_rad < right.angle_rad;
        }
    );

    const std::size_t no_chain = std::numeric_limits<std::size_t>::max();
    std::vector<std::size_t> next_chain(chain_count, no_chain);
    if (!validate_endpoint_order(
            endpoints,
            interior_side,
            angular_tolerance(options, radius_m),
            next_chain,
            result
        )) {
        return false;
    }

    std::vector<bool> visited(chain_count, false);
    for (std::size_t first = 0U; first < chain_count; ++first) {
        if (visited[first]) {
            continue;
        }
        if (result.rings.size() >= options.max_output_rings) {
            result.error = GlobePolygonError::output_ring_limit_exceeded;
            return false;
        }

        std::vector<geometry::PlanarPoint> output = curve.visible_parts[first];
        visited[first] = true;
        std::size_t current = first;
        std::size_t guard = 0U;

        while (true) {
            const std::size_t next = next_chain[current];
            if (next == no_chain || next >= chain_count) {
                result.error = GlobePolygonError::ambiguous_horizon_topology;
                return false;
            }

            const double start_angle = limb_angle(output.back());
            const double end_angle = limb_angle(
                curve.visible_parts[next].front()
            );
            if (!append_limb_arc(
                    output,
                    start_angle,
                    end_angle,
                    interior_side,
                    options,
                    radius_m,
                    result
                )) {
                return false;
            }

            if (next == first) {
                break;
            }
            if (visited[next]) {
                result.error = GlobePolygonError::ambiguous_horizon_topology;
                return false;
            }
            visited[next] = true;

            const auto& next_points = curve.visible_parts[next];
            if (next_points.size() > 1U) {
                output.insert(
                    output.end(),
                    next_points.begin() + 1,
                    next_points.end()
                );
            }

            current = next;
            ++guard;
            if (guard > chain_count) {
                result.error = GlobePolygonError::ambiguous_horizon_topology;
                return false;
            }
        }

        remove_duplicate_terminal(output, radius_m);
        if (output.size() < 3U) {
            result.error = GlobePolygonError::ambiguous_horizon_topology;
            return false;
        }
        result.rings.push_back(std::move(output));
    }

    return true;
}

[[nodiscard]] bool finalize_result(
    const geometry::RingInteriorSide interior_side,
    const double radius_m,
    GlobePolygonResult& result
) noexcept {
    long double area_sum = 0.0L;
    result.projected_vertices = 0U;

    for (const auto& ring : result.rings) {
        const double area = geometry::signed_planar_area(ring);
        if (!std::isfinite(area)) {
            result.error = GlobePolygonError::non_finite_planar_area;
            return false;
        }
        area_sum += static_cast<long double>(area);
        result.projected_vertices += ring.size();
    }

    result.planar_signed_area_m2 = static_cast<double>(area_sum);
    result.visible_disk_area_m2 = geo::kPi * radius_m * radius_m;
    if (!std::isfinite(result.planar_signed_area_m2) ||
        !std::isfinite(result.visible_disk_area_m2)) {
        result.error = GlobePolygonError::non_finite_planar_area;
        return false;
    }

    const double roundoff =
        512.0 * std::numeric_limits<double>::epsilon() *
        (result.visible_disk_area_m2 +
         std::abs(result.planar_signed_area_m2) + 1.0);
    if (std::abs(result.planar_signed_area_m2) >
        result.visible_disk_area_m2 + roundoff) {
        result.error = GlobePolygonError::planar_area_out_of_range;
        return false;
    }

    if (!result.rings.empty() &&
        std::abs(result.planar_signed_area_m2) > roundoff) {
        const bool positive = result.planar_signed_area_m2 > 0.0;
        const bool expected_positive =
            interior_side == geometry::RingInteriorSide::left;
        if (positive != expected_positive) {
            result.error = GlobePolygonError::orientation_mismatch;
            return false;
        }
    }

    return true;
}

}  // namespace

GlobePolygonResult project_visible_wgs84_linear_polygon_ring(
    const geometry::LinearRing& ring,
    const geo::Mat3& world_to_view,
    const GlobePolygonOptions& options,
    const double radius_m
) {
    GlobePolygonResult result{};

    if (!valid_options(options, world_to_view, radius_m)) {
        result.error = GlobePolygonError::invalid_options;
        return result;
    }
    if (!valid_ring(ring)) {
        result.error = GlobePolygonError::invalid_geometry;
        return result;
    }
    if (ring.interior_side == geometry::RingInteriorSide::unspecified) {
        result.error = GlobePolygonError::missing_interior_side;
        return result;
    }

    const geometry::GeographicAreaResult source_area =
        geometry::signed_wgs84_linear_ring_area(ring);
    if (!source_area.ok()) {
        result.error = GlobePolygonError::geographic_area_failed;
        result.geographic_error = source_area.error;
        return result;
    }
    result.source_signed_area_m2 = source_area.signed_area_m2;

    const HemisphereCoverage coverage = classify_coverage(
        source_area,
        radius_m
    );
    if (coverage == HemisphereCoverage::ambiguous) {
        result.error = GlobePolygonError::ambiguous_hemisphere_coverage;
        return result;
    }

    const GlobeCurveResult curve = project_visible_wgs84_linear_ring(
        ring,
        world_to_view,
        options.curve,
        radius_m
    );
    if (!curve.ok()) {
        result.error = GlobePolygonError::curve_failed;
        result.curve_error = curve.error;
        result.sample_error = curve.sample_error;
        return result;
    }
    result.horizon_crossings = curve.horizon_crossings;

    if (curve.horizon_crossings == 0U) {
        if (curve.visible_parts.empty()) {
            if (coverage == HemisphereCoverage::major &&
                !append_full_limb(
                    ring.interior_side,
                    options,
                    radius_m,
                    result
                )) {
                return result;
            }
            static_cast<void>(finalize_result(
                ring.interior_side,
                radius_m,
                result
            ));
            return result;
        }

        if (curve.visible_parts.size() != 1U ||
            !same_point(
                curve.visible_parts.front().front(),
                curve.visible_parts.front().back(),
                radius_m
            )) {
            result.error = GlobePolygonError::ambiguous_horizon_topology;
            return result;
        }

        if (coverage == HemisphereCoverage::major &&
            !append_full_limb(
                ring.interior_side,
                options,
                radius_m,
                result
            )) {
            return result;
        }
        if (!add_closed_source_boundary(
                curve.visible_parts.front(),
                options,
                radius_m,
                result
            )) {
            return result;
        }

        static_cast<void>(finalize_result(
            ring.interior_side,
            radius_m,
            result
        ));
        return result;
    }

    if ((curve.horizon_crossings % 2U) != 0U ||
        !build_partial_rings(
            curve,
            ring.interior_side,
            options,
            radius_m,
            result
        )) {
        if (result.error == GlobePolygonError::none) {
            result.error = GlobePolygonError::ambiguous_horizon_topology;
        }
        return result;
    }

    static_cast<void>(finalize_result(
        ring.interior_side,
        radius_m,
        result
    ));
    return result;
}

}  // namespace aeris::view
