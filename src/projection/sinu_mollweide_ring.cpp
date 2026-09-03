// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/projection/sinu_mollweide_ring.hpp"

#include "aeris/geo/rotation.hpp"
#include "aeris/projection/sinu_mollweide.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace aeris::projection {
namespace {

constexpr double kTwoPi = 2.0 * geo::kPi;
constexpr double kMaximumAcceptedLongitudeStep = geo::kPi / 3.0;

struct FrameSample final {
    geometry::GeodeticPoint pseudo_wgs84{};
    geo::Vec3 authalic_vector{};
};

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
           options.subdivision_max_segments_per_edge > 0U &&
           options.max_projection_pieces > 0U;
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

[[nodiscard]] bool transform_to_philbrick_frame(
    const geometry::GeodeticPoint source,
    FrameSample& output,
    geo::MathError& error
) noexcept {
    const auto beta = geo::authalic_latitude(source.latitude_rad);
    if (!beta.ok()) {
        error = beta.error;
        return false;
    }

    const geo::Vec3 world = geo::lonlat_to_unit_vector(
        source.longitude_rad,
        beta.value
    );
    const geo::Vec3 rotated = geo::apply(
        philbrick_world_to_projection_matrix(),
        world
    );
    const geo::LonLatResult framed = geo::unit_vector_to_lonlat(rotated);
    if (!framed.ok() &&
        framed.error != geo::MathError::indeterminate_coordinate) {
        error = framed.error;
        return false;
    }

    const auto pseudo_geodetic = geo::geodetic_latitude_from_authalic(
        framed.value.latitude_rad
    );
    if (!pseudo_geodetic.ok()) {
        error = pseudo_geodetic.error;
        return false;
    }

    output.pseudo_wgs84 = {
        framed.value.longitude_rad,
        pseudo_geodetic.value,
    };
    output.authalic_vector = rotated;
    return true;
}

[[nodiscard]] double point_to_segment_distance(
    const geo::Vec3 point,
    const geo::Vec3 start,
    const geo::Vec3 end
) noexcept {
    const double dx = end.x - start.x;
    const double dy = end.y - start.y;
    const double dz = end.z - start.z;
    const double length_squared = dx * dx + dy * dy + dz * dz;
    if (!(length_squared > 0.0) || !std::isfinite(length_squared)) {
        return std::hypot(
            std::hypot(point.x - start.x, point.y - start.y),
            point.z - start.z
        );
    }

    const double projection =
        ((point.x - start.x) * dx +
         (point.y - start.y) * dy +
         (point.z - start.z) * dz) /
        length_squared;
    const double clamped = std::clamp(projection, 0.0, 1.0);
    const geo::Vec3 closest{
        start.x + clamped * dx,
        start.y + clamped * dy,
        start.z + clamped * dz,
    };
    return std::hypot(
        std::hypot(point.x - closest.x, point.y - closest.y),
        point.z - closest.z
    );
}

[[nodiscard]] bool longitude_topology_resolved(
    const std::array<FrameSample, 5>& samples
) noexcept {
    for (std::size_t index = 1U; index < samples.size(); ++index) {
        const double delta = std::remainder(
            samples[index].pseudo_wgs84.longitude_rad -
                samples[index - 1U].pseudo_wgs84.longitude_rad,
            kTwoPi
        );
        if (!std::isfinite(delta) ||
            std::abs(delta) >= kMaximumAcceptedLongitudeStep) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool append_frame_edge_recursive(
    const geometry::GeodeticPoint source_start,
    const geometry::GeodeticPoint source_end,
    const double parameter_start,
    const FrameSample& frame_start,
    const double parameter_end,
    const FrameSample& frame_end,
    const double geometric_tolerance_m,
    const unsigned depth,
    const RingProjectionOptions& options,
    std::size_t& emitted_segments,
    std::vector<geometry::GeodeticPoint>& output,
    geo::MathError& sample_error,
    SubdivisionError& subdivision_error
) {
    const double span = parameter_end - parameter_start;
    const double t_quarter = parameter_start + span * 0.25;
    const double t_midpoint = parameter_start + span * 0.5;
    const double t_three_quarter = parameter_start + span * 0.75;
    if (!(t_midpoint > parameter_start) ||
        !(t_midpoint < parameter_end)) {
        subdivision_error = SubdivisionError::limit_exceeded;
        return false;
    }

    const auto source_at = [&](const double parameter) noexcept {
        return geometry::interpolate_wgs84_linear_edge(
            source_start,
            source_end,
            parameter
        );
    };

    std::array<FrameSample, 5> samples{};
    samples[0] = frame_start;
    samples[4] = frame_end;
    if (!transform_to_philbrick_frame(
            source_at(t_quarter),
            samples[1],
            sample_error
        ) ||
        !transform_to_philbrick_frame(
            source_at(t_midpoint),
            samples[2],
            sample_error
        ) ||
        !transform_to_philbrick_frame(
            source_at(t_three_quarter),
            samples[3],
            sample_error
        )) {
        subdivision_error = SubdivisionError::sample_failed;
        return false;
    }

    const double radius_m = geo::authalic_radius_m();
    const double geometric_error_m = radius_m * std::max({
        point_to_segment_distance(
            samples[1].authalic_vector,
            frame_start.authalic_vector,
            frame_end.authalic_vector
        ),
        point_to_segment_distance(
            samples[2].authalic_vector,
            frame_start.authalic_vector,
            frame_end.authalic_vector
        ),
        point_to_segment_distance(
            samples[3].authalic_vector,
            frame_start.authalic_vector,
            frame_end.authalic_vector
        ),
    });
    if (!std::isfinite(geometric_error_m)) {
        sample_error = geo::MathError::numerical_domain_error;
        subdivision_error = SubdivisionError::non_finite_sample;
        return false;
    }

    if (geometric_error_m <= geometric_tolerance_m &&
        longitude_topology_resolved(samples)) {
        if (emitted_segments >= options.subdivision_max_segments_per_edge) {
            subdivision_error = SubdivisionError::limit_exceeded;
            return false;
        }
        ++emitted_segments;
        output.push_back(frame_end.pseudo_wgs84);
        return true;
    }

    if (depth >= options.subdivision_max_depth) {
        subdivision_error = SubdivisionError::limit_exceeded;
        return false;
    }

    return append_frame_edge_recursive(
               source_start,
               source_end,
               parameter_start,
               frame_start,
               t_midpoint,
               samples[2],
               geometric_tolerance_m,
               depth + 1U,
               options,
               emitted_segments,
               output,
               sample_error,
               subdivision_error
           ) &&
           append_frame_edge_recursive(
               source_start,
               source_end,
               t_midpoint,
               samples[2],
               parameter_end,
               frame_end,
               geometric_tolerance_m,
               depth + 1U,
               options,
               emitted_segments,
               output,
               sample_error,
               subdivision_error
           );
}

struct FrameRingResult final {
    geometry::LinearRing ring{};
    geometry::GeographicError geographic_error = geometry::GeographicError::none;
    SubdivisionError subdivision_error = SubdivisionError::none;
    geo::MathError sample_error = geo::MathError::none;
    std::size_t failed_edge = 0U;

    [[nodiscard]] bool ok() const noexcept {
        return geographic_error == geometry::GeographicError::none &&
               subdivision_error == SubdivisionError::none &&
               sample_error == geo::MathError::none;
    }
};

[[nodiscard]] FrameRingResult build_frame_ring(
    const geometry::LinearRing& source,
    const RingProjectionOptions& options,
    const double geometric_tolerance_m
) {
    FrameRingResult result{};
    if (source.vertices.size() < 3U) {
        result.geographic_error = geometry::GeographicError::too_few_vertices;
        return result;
    }

    std::vector<geometry::GeodeticPoint> points;
    points.reserve(source.vertices.size() * 2U + 1U);

    FrameSample first{};
    if (!transform_to_philbrick_frame(
            source.vertices.front(),
            first,
            result.sample_error
        )) {
        result.subdivision_error = SubdivisionError::sample_failed;
        return result;
    }
    points.push_back(first.pseudo_wgs84);

    for (std::size_t edge_index = 0U;
         edge_index < source.vertices.size();
         ++edge_index) {
        const geometry::GeodeticPoint source_start = source.vertices[edge_index];
        const geometry::GeodeticPoint source_end = ring_edge_end(source, edge_index);

        FrameSample frame_start{};
        FrameSample frame_end{};
        if (!transform_to_philbrick_frame(
                source_start,
                frame_start,
                result.sample_error
            ) ||
            !transform_to_philbrick_frame(
                source_end,
                frame_end,
                result.sample_error
            )) {
            result.failed_edge = edge_index;
            result.subdivision_error = SubdivisionError::sample_failed;
            return result;
        }

        std::size_t emitted_segments = 0U;
        if (!append_frame_edge_recursive(
                source_start,
                source_end,
                0.0,
                frame_start,
                1.0,
                frame_end,
                geometric_tolerance_m,
                0U,
                options,
                emitted_segments,
                points,
                result.sample_error,
                result.subdivision_error
            )) {
            result.failed_edge = edge_index;
            return result;
        }
    }

    // The final accepted sample is the physical closure point. Canonicalization
    // owns closure explicitly, so keep all interior samples of the closing edge
    // but remove this duplicate endpoint before rebuilding frame winding.
    if (points.size() < 4U) {
        result.geographic_error = geometry::GeographicError::too_few_vertices;
        return result;
    }
    points.pop_back();

    geometry::LinearRingResult canonical =
        geometry::canonicalize_wgs84_linear_ring(points);
    if (!canonical.ok()) {
        result.geographic_error = canonical.error;
        return result;
    }
    canonical.value.interior_side = source.interior_side;
    result.ring = std::move(canonical.value);
    return result;
}

[[nodiscard]] bool orientation_changed(
    const double source,
    const double candidate
) noexcept {
    if (source == 0.0 || candidate == 0.0) {
        return false;
    }
    return (source < 0.0) != (candidate < 0.0);
}

void copy_projection_failure(
    const PiecewiseRingProjectionResult& projected,
    SinuMollweideRingProjectionResult& result
) noexcept {
    result.projection_error = projected.error;
    result.piece_error = projected.piece_error;
    result.seam_error = projected.seam_error;
    result.geographic_error = projected.geographic_error;
    result.subdivision_error = projected.subdivision_error;
    result.sample_error = projected.sample_error;
    result.failed_piece = projected.failed_piece;
    result.failed_edge = projected.failed_edge;
}

[[nodiscard]] bool projection_failure_may_need_more_frame_budget(
    const PiecewiseRingProjectionResult& projected
) noexcept {
    if (projected.error == PiecewiseRingProjectionError::aggregate_area_budget_unmet) {
        return true;
    }
    return projected.error == PiecewiseRingProjectionError::piece_projection_failed &&
           projected.piece_error == RingProjectionError::area_budget_unmet;
}

void copy_projection_success(
    PiecewiseRingProjectionResult& projected,
    SinuMollweideRingProjectionResult& result
) noexcept {
    result.frame_signed_area_m2 = projected.source_signed_area_m2;
    result.planar_signed_area_m2 = projected.planar_signed_area_m2;
    result.seam_crossings = projected.seam_crossings;
    result.projected_vertices = projected.projected_vertices;
    result.max_projection_refinement_rounds =
        projected.max_piece_refinement_rounds;
}

}  // namespace

SinuMollweideRingProjectionResult
project_philbrick_wgs84_linear_ring_piecewise_verified(
    const geometry::LinearRing& ring,
    const RingProjectionOptions& options
) {
    SinuMollweideRingProjectionResult result{};
    if (!valid_options(options)) {
        result.error = SinuMollweideRingError::invalid_options;
        return result;
    }

    const geometry::GeographicAreaResult source_area =
        geometry::signed_wgs84_linear_ring_area(ring);
    if (!source_area.ok()) {
        result.error = SinuMollweideRingError::source_area_failed;
        result.geographic_error = source_area.error;
        return result;
    }
    result.source_signed_area_m2 = source_area.signed_area_m2;
    result.allowed_area_error_m2 = std::max(
        options.absolute_area_tolerance_m2,
        options.relative_area_tolerance *
            std::abs(result.source_signed_area_m2)
    );
    if (!std::isfinite(result.allowed_area_error_m2) ||
        !(result.allowed_area_error_m2 > 0.0)) {
        result.error = SinuMollweideRingError::invalid_options;
        return result;
    }

    double geometric_tolerance_m = options.initial_geometric_tolerance_m;
    bool frame_ever_fit_total_budget = false;
    bool projection_was_budget_limited = false;

    for (unsigned round = 0U;
         round < options.max_refinement_rounds;
         ++round) {
        FrameRingResult framed = build_frame_ring(
            ring,
            options,
            geometric_tolerance_m
        );
        result.frame_refinement_rounds = round;
        if (!framed.ok()) {
            result.error = SinuMollweideRingError::frame_transform_failed;
            result.geographic_error = framed.geographic_error;
            result.subdivision_error = framed.subdivision_error;
            result.sample_error = framed.sample_error;
            result.failed_edge = framed.failed_edge;
            return result;
        }

        const geometry::GeographicAreaResult frame_area =
            geometry::signed_wgs84_linear_ring_area(framed.ring);
        if (!frame_area.ok()) {
            result.error = SinuMollweideRingError::frame_transform_failed;
            result.geographic_error = frame_area.error;
            return result;
        }

        result.frame_signed_area_m2 = frame_area.signed_area_m2;
        result.frame_absolute_area_error_m2 = std::abs(
            result.frame_signed_area_m2 - result.source_signed_area_m2
        );
        if (!std::isfinite(result.frame_absolute_area_error_m2)) {
            result.error = SinuMollweideRingError::frame_transform_failed;
            return result;
        }

        const bool frame_orientation_ok = !orientation_changed(
            result.source_signed_area_m2,
            result.frame_signed_area_m2
        );
        if (frame_orientation_ok &&
            result.frame_absolute_area_error_m2 < result.allowed_area_error_m2) {
            frame_ever_fit_total_budget = true;
            const double remaining_budget =
                result.allowed_area_error_m2 - result.frame_absolute_area_error_m2;

            RingProjectionOptions projection_options = options;
            projection_options.primitive = EqualAreaPrimitive::sinu_mollweide;
            projection_options.relative_area_tolerance = 0.0;
            projection_options.absolute_area_tolerance_m2 = remaining_budget;

            PiecewiseRingProjectionResult projected =
                project_wgs84_linear_ring_piecewise_verified(
                    framed.ring,
                    projection_options
                );
            if (projected.ok()) {
                copy_projection_success(projected, result);
                result.absolute_area_error_m2 = std::abs(
                    result.planar_signed_area_m2 - result.source_signed_area_m2
                );
                if (std::isfinite(result.absolute_area_error_m2) &&
                    result.absolute_area_error_m2 <= result.allowed_area_error_m2 &&
                    !orientation_changed(
                        result.source_signed_area_m2,
                        result.planar_signed_area_m2
                    )) {
                    result.pieces = std::move(projected.pieces);
                    return result;
                }

                // The piecewise projector verified itself against the framed
                // ring. If the aggregate comparison against the original ring
                // still misses, a more accurate frame is the only admissible
                // way to recover; the total user-facing budget is never widened.
                projection_was_budget_limited = true;
            } else {
                copy_projection_failure(projected, result);
                if (!projection_failure_may_need_more_frame_budget(projected)) {
                    result.error = SinuMollweideRingError::projection_failed;
                    return result;
                }
                projection_was_budget_limited = true;
            }
        }

        geometric_tolerance_m *= 0.5;
        if (!std::isfinite(geometric_tolerance_m) ||
            !(geometric_tolerance_m > 0.0)) {
            break;
        }
    }

    if (!frame_ever_fit_total_budget) {
        result.error = SinuMollweideRingError::frame_area_budget_unmet;
        return result;
    }

    result.error = projection_was_budget_limited
        ? SinuMollweideRingError::aggregate_area_budget_unmet
        : SinuMollweideRingError::frame_area_budget_unmet;
    return result;
}

}  // namespace aeris::projection
