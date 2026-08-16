// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/view/globe_polygon.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace aeris::view {
namespace {

struct ComponentAssessment final {
    bool acceptable = false;
    double resolution_floor_m2 = 0.0;
    std::size_t significant_count = 0U;
    std::size_t negligible_count = 0U;
    // 1 = significant component with the required orientation.
    // 0 = component whose signed area is unresolved below the binary64 floor.
    std::vector<unsigned char> significance;
};

[[nodiscard]] bool valid_verification_options(
    const VerifiedGlobePolygonOptions& options,
    const double radius_m
) noexcept {
    return std::isfinite(radius_m) && radius_m > 0.0 &&
           std::isfinite(options.relative_area_stability_tolerance) &&
           options.relative_area_stability_tolerance >= 0.0 &&
           std::isfinite(options.absolute_area_stability_tolerance_m2) &&
           options.absolute_area_stability_tolerance_m2 > 0.0 &&
           options.max_refinement_rounds >= 2U;
}

[[nodiscard]] double signed_area_roundoff_floor(
    const GlobePolygonResult& polygon
) noexcept {
    return
        2048.0 * std::numeric_limits<double>::epsilon() *
        std::max({
            1.0,
            std::abs(polygon.planar_signed_area_m2),
            std::abs(polygon.visible_disk_area_m2),
        });
}

[[nodiscard]] ComponentAssessment assess_components(
    const GlobePolygonResult& polygon,
    const geometry::RingInteriorSide interior_side
) {
    ComponentAssessment assessment{};
    assessment.resolution_floor_m2 = signed_area_roundoff_floor(polygon);

    // Zero-crossing major/complement representations intentionally contain a
    // full-limb outer ring plus a source-boundary hole. Those structural rings
    // may have opposite signs and are verified by aggregate orientation.
    if (polygon.horizon_crossings == 0U) {
        assessment.acceptable = true;
        assessment.significant_count = polygon.rings.size();
        assessment.significance.assign(polygon.rings.size(), 1U);
        return assessment;
    }

    if (polygon.rings.empty() ||
        interior_side == geometry::RingInteriorSide::unspecified ||
        !std::isfinite(assessment.resolution_floor_m2) ||
        assessment.resolution_floor_m2 < 0.0) {
        return assessment;
    }

    assessment.significance.reserve(polygon.rings.size());
    const bool expected_positive =
        interior_side == geometry::RingInteriorSide::left;

    for (const auto& ring : polygon.rings) {
        const double area = geometry::signed_planar_area(ring);
        if (!std::isfinite(area)) {
            return assessment;
        }

        if (std::abs(area) <= assessment.resolution_floor_m2) {
            ++assessment.negligible_count;
            assessment.significance.push_back(0U);
            continue;
        }

        if ((area > 0.0) != expected_positive) {
            return assessment;
        }

        ++assessment.significant_count;
        assessment.significance.push_back(1U);
    }

    assessment.acceptable = true;
    return assessment;
}

[[nodiscard]] double allowed_area_delta(
    const double previous_area,
    const GlobePolygonResult& current,
    const VerifiedGlobePolygonOptions& options
) noexcept {
    const double scale = std::max({
        1.0,
        std::abs(previous_area),
        std::abs(current.planar_signed_area_m2),
    });
    const double requested = std::max(
        options.absolute_area_stability_tolerance_m2,
        options.relative_area_stability_tolerance * scale
    );
    const double floating_floor =
        2048.0 * std::numeric_limits<double>::epsilon() *
        std::max(scale, std::abs(current.visible_disk_area_m2));
    return std::max(requested, floating_floor);
}

}  // namespace

VerifiedGlobePolygonResult project_visible_wgs84_linear_polygon_ring_verified(
    const geometry::LinearRing& ring,
    const geo::Mat3& world_to_view,
    const VerifiedGlobePolygonOptions& options,
    const double radius_m
) {
    VerifiedGlobePolygonResult verified{};

    if (!valid_verification_options(options, radius_m)) {
        verified.error = VerifiedGlobePolygonError::invalid_options;
        return verified;
    }

    GlobePolygonOptions finite_options = options.initial;
    double curve_tolerance = finite_options.curve.geometric_tolerance_m;
    double arc_tolerance = finite_options.horizon_arc_tolerance_m;

    bool have_previous_candidate = false;
    double previous_area = 0.0;
    std::size_t previous_crossings = 0U;
    std::size_t previous_ring_count = 0U;
    std::vector<unsigned char> previous_significance;

    VerifiedGlobePolygonError unresolved =
        VerifiedGlobePolygonError::area_convergence_unmet;

    for (unsigned round = 1U;
         round <= options.max_refinement_rounds;
         ++round) {
        if (!std::isfinite(curve_tolerance) || curve_tolerance <= 0.0 ||
            !std::isfinite(arc_tolerance) || arc_tolerance <= 0.0) {
            verified.error = VerifiedGlobePolygonError::invalid_options;
            return verified;
        }

        finite_options.curve.geometric_tolerance_m = curve_tolerance;
        finite_options.horizon_arc_tolerance_m = arc_tolerance;

        GlobePolygonResult current =
            project_visible_wgs84_linear_polygon_ring(
                ring,
                world_to_view,
                finite_options,
                radius_m
            );

        verified.polygon = current;
        verified.refinement_rounds = round;
        verified.final_curve_geometric_tolerance_m = curve_tolerance;
        verified.final_horizon_arc_tolerance_m = arc_tolerance;
        verified.topology_stable = false;
        verified.component_orientation_stable = false;
        verified.estimated_planar_area_error_m2 = 0.0;
        verified.allowed_planar_area_delta_m2 = 0.0;
        verified.component_area_resolution_floor_m2 = 0.0;
        verified.significant_component_count = 0U;
        verified.negligible_component_count = 0U;

        if (!current.ok()) {
            if (current.error == GlobePolygonError::invalid_options) {
                verified.error = VerifiedGlobePolygonError::invalid_options;
                return verified;
            }

            // A finite approximation can invert a very thin visible sliver at
            // the horizon even when all sampled points and roots are valid.
            // Aggregate orientation mismatch is therefore refineable; every
            // other low-level error remains fail-closed.
            if (current.error != GlobePolygonError::orientation_mismatch) {
                verified.error =
                    VerifiedGlobePolygonError::finite_projection_failed;
                return verified;
            }

            unresolved =
                VerifiedGlobePolygonError::component_orientation_unstable;
            have_previous_candidate = false;
            previous_significance.clear();
        } else {
            const ComponentAssessment assessment =
                assess_components(current, ring.interior_side);
            verified.component_area_resolution_floor_m2 =
                assessment.resolution_floor_m2;
            verified.significant_component_count = assessment.significant_count;
            verified.negligible_component_count = assessment.negligible_count;

            if (!assessment.acceptable) {
                unresolved =
                    VerifiedGlobePolygonError::component_orientation_unstable;
                have_previous_candidate = false;
                previous_significance.clear();
            } else {
                verified.component_orientation_stable = true;

                if (!have_previous_candidate) {
                    previous_area = current.planar_signed_area_m2;
                    previous_crossings = current.horizon_crossings;
                    previous_ring_count = current.rings.size();
                    previous_significance = assessment.significance;
                    have_previous_candidate = true;
                    unresolved =
                        VerifiedGlobePolygonError::area_convergence_unmet;
                } else {
                    const bool topology_stable =
                        current.horizon_crossings == previous_crossings &&
                        current.rings.size() == previous_ring_count &&
                        assessment.significance == previous_significance;

                    if (!topology_stable) {
                        previous_area = current.planar_signed_area_m2;
                        previous_crossings = current.horizon_crossings;
                        previous_ring_count = current.rings.size();
                        previous_significance = assessment.significance;
                        unresolved =
                            VerifiedGlobePolygonError::topology_unstable;
                    } else {
                        verified.topology_stable = true;
                        verified.estimated_planar_area_error_m2 =
                            std::abs(
                                current.planar_signed_area_m2 - previous_area
                            );
                        verified.allowed_planar_area_delta_m2 =
                            allowed_area_delta(
                                previous_area,
                                current,
                                options
                            );

                        if (std::isfinite(
                                verified.estimated_planar_area_error_m2
                            ) &&
                            std::isfinite(
                                verified.allowed_planar_area_delta_m2
                            ) &&
                            verified.estimated_planar_area_error_m2 <=
                                verified.allowed_planar_area_delta_m2) {
                            verified.error = VerifiedGlobePolygonError::none;
                            return verified;
                        }

                        previous_area = current.planar_signed_area_m2;
                        previous_crossings = current.horizon_crossings;
                        previous_ring_count = current.rings.size();
                        previous_significance = assessment.significance;
                        unresolved =
                            VerifiedGlobePolygonError::area_convergence_unmet;
                    }
                }
            }
        }

        if (round == options.max_refinement_rounds) {
            break;
        }

        const double next_curve = curve_tolerance * 0.5;
        const double next_arc = arc_tolerance * 0.5;
        if (next_curve == curve_tolerance || next_arc == arc_tolerance) {
            break;
        }
        curve_tolerance = next_curve;
        arc_tolerance = next_arc;
    }

    verified.error = unresolved;
    return verified;
}

}  // namespace aeris::view
