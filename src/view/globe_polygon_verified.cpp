// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/view/globe_polygon.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace aeris::view {
namespace {

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

[[nodiscard]] bool component_orientations_match(
    const GlobePolygonResult& polygon,
    const geometry::RingInteriorSide interior_side
) noexcept {
    // Zero-crossing major/complement representations intentionally contain a
    // full-limb outer ring plus a source-boundary hole. Those structural rings
    // may have opposite signs and are verified by their aggregate orientation.
    if (polygon.horizon_crossings == 0U) {
        return true;
    }

    if (polygon.rings.empty() ||
        interior_side == geometry::RingInteriorSide::unspecified) {
        return false;
    }

    const double floor = signed_area_roundoff_floor(polygon);
    const bool expected_positive =
        interior_side == geometry::RingInteriorSide::left;

    for (const auto& ring : polygon.rings) {
        const double area = geometry::signed_planar_area(ring);
        if (!std::isfinite(area) || std::abs(area) <= floor) {
            return false;
        }

        if ((area > 0.0) != expected_positive) {
            return false;
        }
    }

    return true;
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

        if (!current.ok()) {
            if (current.error == GlobePolygonError::invalid_options) {
                verified.error = VerifiedGlobePolygonError::invalid_options;
                return verified;
            }

            // A finite approximation can invert a very thin visible sliver at
            // the horizon even when all sampled points and roots are valid.
            // Orientation mismatch is therefore refineable; every other
            // low-level error remains fail-closed.
            if (current.error != GlobePolygonError::orientation_mismatch) {
                verified.error =
                    VerifiedGlobePolygonError::finite_projection_failed;
                return verified;
            }

            unresolved =
                VerifiedGlobePolygonError::component_orientation_unstable;
            have_previous_candidate = false;
        } else if (!component_orientations_match(
                       current,
                       ring.interior_side
                   )) {
            unresolved =
                VerifiedGlobePolygonError::component_orientation_unstable;
            have_previous_candidate = false;
        } else {
            verified.component_orientation_stable = true;

            if (!have_previous_candidate) {
                previous_area = current.planar_signed_area_m2;
                previous_crossings = current.horizon_crossings;
                previous_ring_count = current.rings.size();
                have_previous_candidate = true;
                unresolved =
                    VerifiedGlobePolygonError::area_convergence_unmet;
            } else {
                const bool topology_stable =
                    current.horizon_crossings == previous_crossings &&
                    current.rings.size() == previous_ring_count;

                if (!topology_stable) {
                    previous_area = current.planar_signed_area_m2;
                    previous_crossings = current.horizon_crossings;
                    previous_ring_count = current.rings.size();
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
                    unresolved =
                        VerifiedGlobePolygonError::area_convergence_unmet;
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
