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
    std::vector<unsigned char> significance;
    std::vector<double> signed_areas_m2;
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
    assessment.significance.reserve(polygon.rings.size());
    assessment.signed_areas_m2.reserve(polygon.rings.size());

    if (!std::isfinite(assessment.resolution_floor_m2) ||
        assessment.resolution_floor_m2 < 0.0) {
        return assessment;
    }

    const bool structural_zero_crossing = polygon.horizon_crossings == 0U;
    if (!structural_zero_crossing &&
        (polygon.rings.empty() ||
         interior_side == geometry::RingInteriorSide::unspecified)) {
        return assessment;
    }

    const bool expected_positive =
        interior_side == geometry::RingInteriorSide::left;

    for (const auto& ring : polygon.rings) {
        const double area = geometry::signed_planar_area(ring);
        if (!std::isfinite(area)) {
            return assessment;
        }
        assessment.signed_areas_m2.push_back(area);

        // Zero-crossing major/complement representations intentionally contain
        // structural outer/hole rings of opposite signs. Their aggregate sign
        // is validated by the low-level projector, so they remain significant
        // without a per-ring orientation requirement.
        if (structural_zero_crossing) {
            ++assessment.significant_count;
            assessment.significance.push_back(1U);
            continue;
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
    const double current_area,
    const double numerical_floor,
    const VerifiedGlobePolygonOptions& options
) noexcept {
    const double scale = std::max({
        1.0,
        std::abs(previous_area),
        std::abs(current_area),
    });
    const double requested = std::max(
        options.absolute_area_stability_tolerance_m2,
        options.relative_area_stability_tolerance * scale
    );
    return std::max(requested, numerical_floor);
}

[[nodiscard]] bool component_areas_converged(
    const std::vector<double>& previous,
    const std::vector<double>& current,
    const double previous_floor,
    const double current_floor,
    const VerifiedGlobePolygonOptions& options,
    double& max_error_m2
) noexcept {
    max_error_m2 = 0.0;
    if (previous.size() != current.size()) {
        return false;
    }

    const double numerical_floor = std::max(previous_floor, current_floor);
    for (std::size_t index = 0U; index < current.size(); ++index) {
        if (!std::isfinite(previous[index]) || !std::isfinite(current[index])) {
            return false;
        }
        const double delta = std::abs(current[index] - previous[index]);
        max_error_m2 = std::max(max_error_m2, delta);
        if (delta > allowed_area_delta(
                        previous[index],
                        current[index],
                        numerical_floor,
                        options)) {
            return false;
        }
    }
    return true;
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
    double previous_component_floor = 0.0;
    std::size_t previous_crossings = 0U;
    std::size_t previous_ring_count = 0U;
    std::vector<unsigned char> previous_significance;
    std::vector<double> previous_component_areas;

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
        verified.component_area_stable = false;
        verified.estimated_planar_area_error_m2 = 0.0;
        verified.allowed_planar_area_delta_m2 = 0.0;
        verified.estimated_max_component_area_error_m2 = 0.0;
        verified.component_area_resolution_floor_m2 = 0.0;
        verified.significant_component_count = 0U;
        verified.negligible_component_count = 0U;

        if (!current.ok()) {
            if (current.error == GlobePolygonError::invalid_options) {
                verified.error = VerifiedGlobePolygonError::invalid_options;
                return verified;
            }

            if (current.error != GlobePolygonError::orientation_mismatch) {
                verified.error =
                    VerifiedGlobePolygonError::finite_projection_failed;
                return verified;
            }

            unresolved =
                VerifiedGlobePolygonError::component_orientation_unstable;
            have_previous_candidate = false;
            previous_significance.clear();
            previous_component_areas.clear();
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
                previous_component_areas.clear();
            } else {
                verified.component_orientation_stable = true;

                if (!have_previous_candidate) {
                    previous_area = current.planar_signed_area_m2;
                    previous_component_floor = assessment.resolution_floor_m2;
                    previous_crossings = current.horizon_crossings;
                    previous_ring_count = current.rings.size();
                    previous_significance = assessment.significance;
                    previous_component_areas = assessment.signed_areas_m2;
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
                        previous_component_floor = assessment.resolution_floor_m2;
                        previous_crossings = current.horizon_crossings;
                        previous_ring_count = current.rings.size();
                        previous_significance = assessment.significance;
                        previous_component_areas = assessment.signed_areas_m2;
                        unresolved =
                            VerifiedGlobePolygonError::topology_unstable;
                    } else {
                        verified.topology_stable = true;

                        const bool components_stable = component_areas_converged(
                            previous_component_areas,
                            assessment.signed_areas_m2,
                            previous_component_floor,
                            assessment.resolution_floor_m2,
                            options,
                            verified.estimated_max_component_area_error_m2
                        );
                        verified.component_area_stable = components_stable;

                        verified.estimated_planar_area_error_m2 =
                            std::abs(
                                current.planar_signed_area_m2 - previous_area
                            );
                        const double aggregate_floor = std::max(
                            previous_component_floor,
                            assessment.resolution_floor_m2
                        );
                        verified.allowed_planar_area_delta_m2 =
                            allowed_area_delta(
                                previous_area,
                                current.planar_signed_area_m2,
                                aggregate_floor,
                                options
                            );

                        const bool aggregate_stable =
                            std::isfinite(
                                verified.estimated_planar_area_error_m2
                            ) &&
                            std::isfinite(
                                verified.allowed_planar_area_delta_m2
                            ) &&
                            verified.estimated_planar_area_error_m2 <=
                                verified.allowed_planar_area_delta_m2;

                        if (components_stable && aggregate_stable) {
                            verified.error = VerifiedGlobePolygonError::none;
                            return verified;
                        }

                        previous_area = current.planar_signed_area_m2;
                        previous_component_floor = assessment.resolution_floor_m2;
                        previous_crossings = current.horizon_crossings;
                        previous_ring_count = current.rings.size();
                        previous_significance = assessment.significance;
                        previous_component_areas = assessment.signed_areas_m2;
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
