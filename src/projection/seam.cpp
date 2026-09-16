// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/projection/seam.hpp"

#include "aeris/geo/wgs84.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace aeris::projection {
namespace {

constexpr double kTwoPi = 2.0 * geo::kPi;
constexpr double kAngularTolerance =
    1024.0 * std::numeric_limits<double>::epsilon() * geo::kPi;
constexpr double kParameterTolerance =
    128.0 * std::numeric_limits<double>::epsilon();

struct Chain final {
    std::vector<geometry::GeodeticPoint> points;
};

enum class BoundarySide {
    none = 0,
    left,
    right,
};

struct BoundaryEndpoint final {
    std::size_t chain_index = 0U;
    bool is_start = false;
    double latitude_rad = 0.0;
};

struct StripClipResult final {
    std::vector<geometry::LinearRing> pieces;
    std::size_t seam_crossing_incidences = 0U;
    SeamSplitError error = SeamSplitError::none;
};

[[nodiscard]] bool finite_ring(const geometry::LinearRing& ring) noexcept {
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

[[nodiscard]] bool nearly_equal(
    const double left,
    const double right,
    const double tolerance = kAngularTolerance
) noexcept {
    return std::abs(left - right) <= tolerance;
}

[[nodiscard]] bool same_point(
    const geometry::GeodeticPoint left,
    const geometry::GeodeticPoint right
) noexcept {
    return nearly_equal(left.longitude_rad, right.longitude_rad) &&
           nearly_equal(left.latitude_rad, right.latitude_rad);
}

[[nodiscard]] bool longitude_inside_strip(
    const double longitude_rad,
    const double left,
    const double right
) noexcept {
    return longitude_rad >= left - kAngularTolerance &&
           longitude_rad <= right + kAngularTolerance;
}

[[nodiscard]] double snap_longitude_to_strip(
    const double longitude_rad,
    const double left,
    const double right
) noexcept {
    if (nearly_equal(longitude_rad, left)) {
        return left;
    }
    if (nearly_equal(longitude_rad, right)) {
        return right;
    }
    return longitude_rad;
}

[[nodiscard]] bool whole_ring_fits_one_branch(
    const geometry::LinearRing& ring,
    const double central_meridian_rad,
    double& longitude_shift_rad
) noexcept {
    double minimum = ring.closing_longitude_rad;
    double maximum = ring.closing_longitude_rad;
    for (const geometry::GeodeticPoint point : ring.vertices) {
        minimum = std::min(minimum, point.longitude_rad);
        maximum = std::max(maximum, point.longitude_rad);
    }

    if (!std::isfinite(minimum) || !std::isfinite(maximum)) {
        return false;
    }

    const double midpoint = minimum + 0.5 * (maximum - minimum);
    const double turns = std::round((central_meridian_rad - midpoint) / kTwoPi);
    if (!std::isfinite(turns)) {
        return false;
    }

    const double shift = turns * kTwoPi;
    const double domain_left = central_meridian_rad - geo::kPi;
    const double domain_right = central_meridian_rad + geo::kPi;

    if (!longitude_inside_strip(
            ring.closing_longitude_rad + shift,
            domain_left,
            domain_right
        )) {
        return false;
    }
    for (const geometry::GeodeticPoint point : ring.vertices) {
        if (!longitude_inside_strip(
                point.longitude_rad + shift,
                domain_left,
                domain_right
            )) {
            return false;
        }
    }

    longitude_shift_rad = shift;
    return true;
}

[[nodiscard]] geometry::LinearRing shifted_ring(
    const geometry::LinearRing& ring,
    const double shift
) {
    geometry::LinearRing result{};
    result.vertices.reserve(ring.vertices.size());
    for (const geometry::GeodeticPoint point : ring.vertices) {
        result.vertices.push_back({
            point.longitude_rad + shift,
            point.latitude_rad,
        });
    }
    result.closing_longitude_rad = ring.closing_longitude_rad + shift;
    result.longitude_winding = ring.longitude_winding;
    result.interior_side = ring.interior_side;
    return result;
}

[[nodiscard]] bool edge_coincident_with_boundary(
    const geometry::GeodeticPoint start,
    const geometry::GeodeticPoint end,
    const double left,
    const double right
) noexcept {
    if (!nearly_equal(start.longitude_rad, end.longitude_rad)) {
        return false;
    }
    return nearly_equal(start.longitude_rad, left) ||
           nearly_equal(start.longitude_rad, right);
}

[[nodiscard]] bool coincident_edge_belongs_to_strip(
    const geometry::GeodeticPoint start,
    const geometry::GeodeticPoint end,
    const double left,
    const double right,
    const geometry::RingInteriorSide interior_side
) noexcept {
    if (!edge_coincident_with_boundary(start, end, left, right) ||
        interior_side == geometry::RingInteriorSide::unspecified ||
        nearly_equal(start.latitude_rad, end.latitude_rad)) {
        return false;
    }

    const bool on_left_boundary = nearly_equal(start.longitude_rad, left);
    const bool on_right_boundary = nearly_equal(start.longitude_rad, right);
    if (on_left_boundary == on_right_boundary) {
        return false;
    }

    // For a north-going meridian edge, the geometric left side is west and
    // the right side is east. South-going reverses that relationship. A strip
    // owns the coincident edge iff its interior half-plane is the polygon's
    // declared interior half-plane. This assigns a seam boundary to exactly
    // one adjacent strip without perturbing the requested cut.
    const bool northward = end.latitude_rad > start.latitude_rad;
    const bool polygon_interior_is_west = northward
        ? interior_side == geometry::RingInteriorSide::left
        : interior_side == geometry::RingInteriorSide::right;
    const bool strip_interior_is_west = on_right_boundary;
    return polygon_interior_is_west == strip_interior_is_west;
}

[[nodiscard]] bool clip_edge_to_strip(
    const geometry::GeodeticPoint start,
    const geometry::GeodeticPoint end,
    const double left,
    const double right,
    geometry::GeodeticPoint& clipped_start,
    geometry::GeodeticPoint& clipped_end,
    bool& has_segment
) noexcept {
    has_segment = false;

    const double delta = end.longitude_rad - start.longitude_rad;
    if (!std::isfinite(delta)) {
        return false;
    }

    if (delta == 0.0) {
        if (!longitude_inside_strip(start.longitude_rad, left, right)) {
            return true;
        }
        clipped_start = start;
        clipped_end = end;
        clipped_start.longitude_rad = snap_longitude_to_strip(
            clipped_start.longitude_rad,
            left,
            right
        );
        clipped_end.longitude_rad = clipped_start.longitude_rad;
        has_segment = !same_point(clipped_start, clipped_end);
        return true;
    }

    const double left_parameter = (left - start.longitude_rad) / delta;
    const double right_parameter = (right - start.longitude_rad) / delta;
    double parameter0 = std::max(
        0.0,
        std::min(left_parameter, right_parameter)
    );
    double parameter1 = std::min(
        1.0,
        std::max(left_parameter, right_parameter)
    );

    if (parameter1 < parameter0 - kParameterTolerance) {
        return true;
    }

    parameter0 = std::clamp(parameter0, 0.0, 1.0);
    parameter1 = std::clamp(parameter1, 0.0, 1.0);
    if (parameter1 <= parameter0 + kParameterTolerance) {
        return true;
    }

    clipped_start = geometry::interpolate_wgs84_linear_edge(
        start,
        end,
        parameter0
    );
    clipped_end = geometry::interpolate_wgs84_linear_edge(
        start,
        end,
        parameter1
    );
    clipped_start.longitude_rad = snap_longitude_to_strip(
        clipped_start.longitude_rad,
        left,
        right
    );
    clipped_end.longitude_rad = snap_longitude_to_strip(
        clipped_end.longitude_rad,
        left,
        right
    );

    has_segment = !same_point(clipped_start, clipped_end);
    return true;
}

void append_segment(
    std::vector<Chain>& chains,
    const geometry::GeodeticPoint start,
    const geometry::GeodeticPoint end
) {
    if (chains.empty() ||
        chains.back().points.empty() ||
        !same_point(chains.back().points.back(), start)) {
        Chain chain{};
        chain.points.push_back(start);
        chain.points.push_back(end);
        chains.push_back(std::move(chain));
        return;
    }

    if (!same_point(chains.back().points.back(), end)) {
        chains.back().points.push_back(end);
    }
}

void merge_cyclic_chains(std::vector<Chain>& chains) {
    if (chains.size() < 2U ||
        chains.front().points.empty() ||
        chains.back().points.empty() ||
        !same_point(chains.back().points.back(), chains.front().points.front())) {
        return;
    }

    Chain merged = std::move(chains.back());
    chains.pop_back();
    if (chains.front().points.size() > 1U) {
        merged.points.insert(
            merged.points.end(),
            chains.front().points.begin() + 1,
            chains.front().points.end()
        );
    }
    chains.front() = std::move(merged);
}

[[nodiscard]] BoundarySide classify_boundary(
    const geometry::GeodeticPoint point,
    const double left,
    const double right
) noexcept {
    if (nearly_equal(point.longitude_rad, left)) {
        return BoundarySide::left;
    }
    if (nearly_equal(point.longitude_rad, right)) {
        return BoundarySide::right;
    }
    return BoundarySide::none;
}

[[nodiscard]] bool pair_boundary_endpoints(
    std::vector<BoundaryEndpoint>& endpoints,
    const bool ascending,
    std::vector<std::size_t>& next_chain,
    SeamSplitError& error
) {
    if (endpoints.empty()) {
        return true;
    }
    if ((endpoints.size() % 2U) != 0U) {
        error = SeamSplitError::ambiguous_seam_touch;
        return false;
    }

    std::sort(
        endpoints.begin(),
        endpoints.end(),
        [ascending](const BoundaryEndpoint& left, const BoundaryEndpoint& right) {
            return ascending
                ? left.latitude_rad < right.latitude_rad
                : left.latitude_rad > right.latitude_rad;
        }
    );

    for (std::size_t index = 1U; index < endpoints.size(); ++index) {
        if (nearly_equal(
                endpoints[index - 1U].latitude_rad,
                endpoints[index].latitude_rad
            )) {
            error = SeamSplitError::ambiguous_seam_touch;
            return false;
        }
    }

    const std::size_t no_chain = std::numeric_limits<std::size_t>::max();
    for (std::size_t index = 0U; index < endpoints.size(); index += 2U) {
        const BoundaryEndpoint end = endpoints[index];
        const BoundaryEndpoint start = endpoints[index + 1U];
        if (end.is_start || !start.is_start ||
            end.chain_index >= next_chain.size() ||
            start.chain_index >= next_chain.size() ||
            next_chain[end.chain_index] != no_chain) {
            error = SeamSplitError::topology_inconsistent;
            return false;
        }
        next_chain[end.chain_index] = start.chain_index;
    }

    return true;
}

[[nodiscard]] geometry::LinearRing make_piece(
    const std::vector<geometry::GeodeticPoint>& points,
    const double shift,
    const geometry::RingInteriorSide interior_side
) {
    geometry::LinearRing piece{};
    piece.vertices.reserve(points.size());
    for (const geometry::GeodeticPoint point : points) {
        piece.vertices.push_back({
            point.longitude_rad + shift,
            point.latitude_rad,
        });
    }
    piece.closing_longitude_rad = piece.vertices.front().longitude_rad;
    piece.longitude_winding = 0;
    piece.interior_side = interior_side;
    return piece;
}

[[nodiscard]] StripClipResult clip_ring_to_strip(
    const geometry::LinearRing& ring,
    const double left,
    const double right,
    const double shift_into_active_domain,
    const std::size_t remaining_piece_budget
) {
    StripClipResult result{};
    std::vector<Chain> chains;

    for (std::size_t edge_index = 0U; edge_index < ring.vertices.size(); ++edge_index) {
        const geometry::GeodeticPoint start = ring.vertices[edge_index];
        const geometry::GeodeticPoint end = ring_edge_end(ring, edge_index);

        if (edge_coincident_with_boundary(start, end, left, right)) {
            if (coincident_edge_belongs_to_strip(
                    start,
                    end,
                    left,
                    right,
                    ring.interior_side
                )) {
                geometry::GeodeticPoint owned_start = start;
                geometry::GeodeticPoint owned_end = end;
                owned_start.longitude_rad = snap_longitude_to_strip(
                    owned_start.longitude_rad,
                    left,
                    right
                );
                owned_end.longitude_rad = owned_start.longitude_rad;
                if (!same_point(owned_start, owned_end)) {
                    append_segment(chains, owned_start, owned_end);
                }
            }
            continue;
        }

        geometry::GeodeticPoint clipped_start{};
        geometry::GeodeticPoint clipped_end{};
        bool has_segment = false;
        if (!clip_edge_to_strip(
                start,
                end,
                left,
                right,
                clipped_start,
                clipped_end,
                has_segment
            )) {
            result.error = SeamSplitError::invalid_ring;
            return result;
        }
        if (!has_segment) {
            continue;
        }

        const bool start_on_boundary =
            classify_boundary(clipped_start, left, right) != BoundarySide::none;
        const bool end_on_boundary =
            classify_boundary(clipped_end, left, right) != BoundarySide::none;
        const bool original_start_inside = longitude_inside_strip(
            start.longitude_rad,
            left,
            right
        );
        const bool original_end_inside = longitude_inside_strip(
            end.longitude_rad,
            left,
            right
        );

        if (start_on_boundary && !original_start_inside) {
            ++result.seam_crossing_incidences;
        }
        if (end_on_boundary && !original_end_inside) {
            ++result.seam_crossing_incidences;
        }

        append_segment(chains, clipped_start, clipped_end);
    }

    if (chains.empty()) {
        return result;
    }

    merge_cyclic_chains(chains);

    if (chains.size() == 1U &&
        chains.front().points.size() >= 4U &&
        same_point(chains.front().points.front(), chains.front().points.back())) {
        chains.front().points.pop_back();
        if (chains.front().points.size() < 3U) {
            result.error = SeamSplitError::topology_inconsistent;
            return result;
        }
        result.pieces.push_back(make_piece(
            chains.front().points,
            shift_into_active_domain,
            ring.interior_side
        ));
        return result;
    }

    std::vector<BoundaryEndpoint> left_endpoints;
    std::vector<BoundaryEndpoint> right_endpoints;
    left_endpoints.reserve(chains.size() * 2U);
    right_endpoints.reserve(chains.size() * 2U);

    for (std::size_t chain_index = 0U; chain_index < chains.size(); ++chain_index) {
        const Chain& chain = chains[chain_index];
        if (chain.points.size() < 2U) {
            result.error = SeamSplitError::topology_inconsistent;
            return result;
        }

        const BoundarySide start_side = classify_boundary(
            chain.points.front(),
            left,
            right
        );
        const BoundarySide end_side = classify_boundary(
            chain.points.back(),
            left,
            right
        );
        if (start_side == BoundarySide::none || end_side == BoundarySide::none) {
            result.error = SeamSplitError::topology_inconsistent;
            return result;
        }

        const BoundaryEndpoint start_endpoint{
            chain_index,
            true,
            chain.points.front().latitude_rad,
        };
        const BoundaryEndpoint end_endpoint{
            chain_index,
            false,
            chain.points.back().latitude_rad,
        };

        (start_side == BoundarySide::left ? left_endpoints : right_endpoints)
            .push_back(start_endpoint);
        (end_side == BoundarySide::left ? left_endpoints : right_endpoints)
            .push_back(end_endpoint);
    }

    const std::size_t no_chain = std::numeric_limits<std::size_t>::max();
    std::vector<std::size_t> next_chain(chains.size(), no_chain);
    SeamSplitError pairing_error = SeamSplitError::none;

    const bool left_ascending =
        ring.interior_side == geometry::RingInteriorSide::right;
    const bool right_ascending =
        ring.interior_side == geometry::RingInteriorSide::left;

    if (!pair_boundary_endpoints(
            left_endpoints,
            left_ascending,
            next_chain,
            pairing_error
        ) ||
        !pair_boundary_endpoints(
            right_endpoints,
            right_ascending,
            next_chain,
            pairing_error
        )) {
        result.error = pairing_error;
        return result;
    }

    std::vector<bool> visited(chains.size(), false);
    for (std::size_t first_chain = 0U; first_chain < chains.size(); ++first_chain) {
        if (visited[first_chain]) {
            continue;
        }
        if (result.pieces.size() >= remaining_piece_budget) {
            result.error = SeamSplitError::piece_limit_exceeded;
            return result;
        }

        std::vector<geometry::GeodeticPoint> piece_points;
        std::size_t current = first_chain;
        std::size_t guard = 0U;

        while (true) {
            if (current >= chains.size() || visited[current]) {
                result.error = SeamSplitError::topology_inconsistent;
                return result;
            }
            visited[current] = true;

            const Chain& chain = chains[current];
            piece_points.insert(
                piece_points.end(),
                chain.points.begin(),
                chain.points.end()
            );

            if (next_chain[current] == no_chain) {
                result.error = SeamSplitError::topology_inconsistent;
                return result;
            }

            const std::size_t next = next_chain[current];
            if (next == first_chain) {
                break;
            }
            current = next;
            ++guard;
            if (guard > chains.size()) {
                result.error = SeamSplitError::topology_inconsistent;
                return result;
            }
        }

        if (piece_points.size() < 3U) {
            result.error = SeamSplitError::topology_inconsistent;
            return result;
        }

        result.pieces.push_back(make_piece(
            piece_points,
            shift_into_active_domain,
            ring.interior_side
        ));
    }

    return result;
}

[[nodiscard]] bool compute_strip_range(
    const geometry::LinearRing& ring,
    const double central_meridian_rad,
    const std::size_t max_pieces,
    long long& first_strip,
    long long& last_strip
) noexcept {
    double minimum = ring.closing_longitude_rad;
    double maximum = ring.closing_longitude_rad;
    for (const geometry::GeodeticPoint point : ring.vertices) {
        minimum = std::min(minimum, point.longitude_rad);
        maximum = std::max(maximum, point.longitude_rad);
    }

    const double base_left = central_meridian_rad - geo::kPi;
    const double first_value = std::floor((minimum - base_left) / kTwoPi) - 1.0;
    const double last_value = std::floor((maximum - base_left) / kTwoPi) + 1.0;
    if (!std::isfinite(first_value) || !std::isfinite(last_value) ||
        first_value < static_cast<double>(std::numeric_limits<long long>::min()) ||
        last_value > static_cast<double>(std::numeric_limits<long long>::max())) {
        return false;
    }

    first_strip = static_cast<long long>(first_value);
    last_strip = static_cast<long long>(last_value);
    if (last_strip < first_strip) {
        return false;
    }

    const long double strip_count =
        static_cast<long double>(last_strip) -
        static_cast<long double>(first_strip) + 1.0L;
    const long double strip_limit =
        static_cast<long double>(max_pieces) + 2.0L;
    return strip_count <= strip_limit;
}

[[nodiscard]] bool verify_area_partition(
    const geometry::LinearRing& source_ring,
    SeamSplitResult& result
) noexcept {
    const geometry::GeographicAreaResult source_area =
        geometry::signed_wgs84_linear_ring_area(source_ring);
    if (!source_area.ok()) {
        result.error = SeamSplitError::geographic_area_failed;
        result.geographic_error = source_area.error;
        return false;
    }

    long double piece_sum = 0.0L;
    long double error_sum = static_cast<long double>(
        source_area.estimated_abs_error_m2
    );

    for (const geometry::LinearRing& piece : result.pieces) {
        const geometry::GeographicAreaResult piece_area =
            geometry::signed_wgs84_linear_ring_area(piece);
        if (!piece_area.ok()) {
            result.error = SeamSplitError::geographic_area_failed;
            result.geographic_error = piece_area.error;
            return false;
        }
        piece_sum += static_cast<long double>(piece_area.signed_area_m2);
        error_sum += static_cast<long double>(piece_area.estimated_abs_error_m2);
    }

    const long double source_value =
        static_cast<long double>(source_area.signed_area_m2);
    const long double difference = std::abs(piece_sum - source_value);
    const long double roundoff =
        128.0L * static_cast<long double>(std::numeric_limits<double>::epsilon()) *
        (std::abs(piece_sum) + std::abs(source_value) + 1.0L);
    const long double bound = error_sum + roundoff;

    result.source_signed_area_m2 = source_area.signed_area_m2;
    result.piece_signed_area_sum_m2 = static_cast<double>(piece_sum);
    result.absolute_area_error_m2 = static_cast<double>(difference);
    result.area_error_bound_m2 = static_cast<double>(bound);

    if (!std::isfinite(result.piece_signed_area_sum_m2) ||
        !std::isfinite(result.absolute_area_error_m2) ||
        !std::isfinite(result.area_error_bound_m2) ||
        difference > bound) {
        result.error = SeamSplitError::area_invariant_failed;
        return false;
    }

    return true;
}

}  // namespace

SeamSplitResult split_wgs84_linear_ring_at_projection_seam(
    const geometry::LinearRing& ring,
    const SeamSplitOptions& options
) {
    SeamSplitResult result{};

    if (!std::isfinite(options.central_meridian_rad) || options.max_pieces == 0U) {
        result.error = SeamSplitError::invalid_options;
        return result;
    }
    if (!finite_ring(ring)) {
        result.error = SeamSplitError::invalid_ring;
        return result;
    }
    if (ring.longitude_winding != 0) {
        result.error = SeamSplitError::nonzero_winding_unsupported;
        return result;
    }

    double whole_ring_shift = 0.0;
    if (whole_ring_fits_one_branch(
            ring,
            options.central_meridian_rad,
            whole_ring_shift
        )) {
        result.pieces.push_back(shifted_ring(ring, whole_ring_shift));
        static_cast<void>(verify_area_partition(ring, result));
        return result;
    }

    if (ring.interior_side == geometry::RingInteriorSide::unspecified) {
        result.error = SeamSplitError::missing_interior_side;
        return result;
    }

    long long first_strip = 0;
    long long last_strip = 0;
    if (!compute_strip_range(
            ring,
            options.central_meridian_rad,
            options.max_pieces,
            first_strip,
            last_strip
        )) {
        result.error = SeamSplitError::piece_limit_exceeded;
        return result;
    }

    const double base_left = options.central_meridian_rad - geo::kPi;
    std::size_t crossing_incidences = 0U;

    for (long long strip = first_strip;; ++strip) {
        const double strip_turns = static_cast<double>(strip) * kTwoPi;
        const double left = base_left + strip_turns;
        const double right = left + kTwoPi;
        const double shift = -strip_turns;

        const std::size_t remaining =
            options.max_pieces - result.pieces.size();
        const StripClipResult clipped = clip_ring_to_strip(
            ring,
            left,
            right,
            shift,
            remaining
        );
        if (clipped.error != SeamSplitError::none) {
            result.error = clipped.error;
            return result;
        }

        crossing_incidences += clipped.seam_crossing_incidences;
        if (result.pieces.size() + clipped.pieces.size() > options.max_pieces) {
            result.error = SeamSplitError::piece_limit_exceeded;
            return result;
        }
        result.pieces.insert(
            result.pieces.end(),
            clipped.pieces.begin(),
            clipped.pieces.end()
        );

        if (strip == last_strip) {
            break;
        }
    }

    if (result.pieces.empty()) {
        result.error = SeamSplitError::topology_inconsistent;
        return result;
    }

    // Every ordinary physical crossing is incident to the two longitude strips
    // that meet at the cut. Boundary-coincident ownership is resolved above and
    // intentionally contributes no synthetic crossing incidence.
    if ((crossing_incidences % 2U) != 0U) {
        result.error = SeamSplitError::ambiguous_seam_touch;
        return result;
    }
    result.seam_crossings = crossing_incidences / 2U;

    static_cast<void>(verify_area_partition(ring, result));
    return result;
}

}  // namespace aeris::projection
