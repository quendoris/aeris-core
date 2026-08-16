// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/geometry/geographic.hpp"

#include "aeris/geo/wgs84.hpp"

#include <cmath>
#include <limits>

namespace aeris::geometry {
namespace {

constexpr double kTwoPi = 2.0 * geo::kPi;

[[nodiscard]] bool ambiguous_half_turn(const double delta) noexcept {
    constexpr double tolerance =
        64.0 * std::numeric_limits<double>::epsilon() * geo::kPi;
    return std::abs(std::abs(delta) - geo::kPi) <= tolerance;
}

[[nodiscard]] bool valid_interior_side(const RingInteriorSide side) noexcept {
    switch (side) {
        case RingInteriorSide::unspecified:
        case RingInteriorSide::left:
        case RingInteriorSide::right:
            return true;
    }
    return false;
}

[[nodiscard]] GeographicError validate_edge_delta(const double delta) noexcept {
    if (!std::isfinite(delta)) return GeographicError::non_finite_coordinate;
    if (ambiguous_half_turn(delta)) return GeographicError::ambiguous_half_turn;
    if (delta < -geo::kPi || delta > geo::kPi) return GeographicError::noncanonical_ring;
    return GeographicError::none;
}

}  // namespace

GeographicError validate_canonical_wgs84_linear_ring(const LinearRing& ring) noexcept {
    if (ring.vertices.size() < 3U) return GeographicError::too_few_vertices;
    if (!valid_interior_side(ring.interior_side)) return GeographicError::noncanonical_ring;

    for (const GeodeticPoint point : ring.vertices) {
        if (!std::isfinite(point.longitude_rad) || !std::isfinite(point.latitude_rad)) {
            return GeographicError::non_finite_coordinate;
        }
        if (point.latitude_rad < -geo::kHalfPi || point.latitude_rad > geo::kHalfPi) {
            return GeographicError::latitude_out_of_range;
        }
    }
    if (!std::isfinite(ring.closing_longitude_rad)) {
        return GeographicError::non_finite_coordinate;
    }

    const double first_longitude = ring.vertices.front().longitude_rad;
    if (!(first_longitude > -geo::kPi && first_longitude <= geo::kPi)) {
        return GeographicError::noncanonical_ring;
    }

    if (ring.vertices.size() >= 4U &&
        ring.vertices.back().latitude_rad == ring.vertices.front().latitude_rad &&
        std::remainder(
            ring.vertices.back().longitude_rad - ring.vertices.front().longitude_rad,
            kTwoPi) == 0.0) {
        return GeographicError::noncanonical_ring;
    }

    for (std::size_t index = 1U; index < ring.vertices.size(); ++index) {
        const GeographicError edge = validate_edge_delta(
            ring.vertices[index].longitude_rad - ring.vertices[index - 1U].longitude_rad);
        if (edge != GeographicError::none) return edge;
    }

    const GeographicError closing_edge = validate_edge_delta(
        ring.closing_longitude_rad - ring.vertices.back().longitude_rad);
    if (closing_edge != GeographicError::none) return closing_edge;

    const double turns = (ring.closing_longitude_rad - first_longitude) / kTwoPi;
    if (!std::isfinite(turns)) return GeographicError::numerical_domain_error;
    const double rounded_turns = std::round(turns);
    constexpr double winding_tolerance = 256.0 * std::numeric_limits<double>::epsilon();
    if (!std::isfinite(rounded_turns) ||
        std::abs(turns - rounded_turns) > winding_tolerance ||
        rounded_turns < static_cast<double>(std::numeric_limits<int>::min()) ||
        rounded_turns > static_cast<double>(std::numeric_limits<int>::max()) ||
        rounded_turns != static_cast<double>(ring.longitude_winding)) {
        return GeographicError::noncanonical_ring;
    }

    if (ring.longitude_winding != 0 && ring.interior_side == RingInteriorSide::unspecified) {
        return GeographicError::longitude_winding_unsupported;
    }
    return GeographicError::none;
}

}  // namespace aeris::geometry
