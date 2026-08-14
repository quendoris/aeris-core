// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/geometry/geographic.hpp"

#include "aeris/geo/wgs84.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace aeris::geometry {
namespace {

constexpr double kTwoPi = 2.0 * geo::kPi;

[[nodiscard]] bool valid_point(const GeodeticPoint point) noexcept {
    return std::isfinite(point.longitude_rad) && std::isfinite(point.latitude_rad) &&
           point.latitude_rad >= -geo::kHalfPi && point.latitude_rad <= geo::kHalfPi;
}

[[nodiscard]] double principal_longitude(const double longitude_rad) noexcept {
    double value = std::remainder(longitude_rad, kTwoPi);
    if (value <= -geo::kPi) {
        value += kTwoPi;
    }
    return value;
}

[[nodiscard]] bool ambiguous_half_turn(const double delta) noexcept {
    constexpr double tolerance =
        64.0 * std::numeric_limits<double>::epsilon() * geo::kPi;
    return std::abs(std::abs(delta) - geo::kPi) <= tolerance;
}

[[nodiscard]] double compensated_add(
    const double value,
    double& sum,
    double& compensation
) noexcept {
    const double corrected = value - compensation;
    const double next = sum + corrected;
    compensation = (next - sum) - corrected;
    sum = next;
    return sum;
}

struct IntegralResult final {
    double value = 0.0;
    double estimated_abs_error = 0.0;
    unsigned deepest_level = 0U;
    GeographicError error = GeographicError::none;

    [[nodiscard]] constexpr bool ok() const noexcept {
        return error == GeographicError::none;
    }
};

[[nodiscard]] double q_on_linear_latitude(
    const double latitude0,
    const double latitude_delta,
    const double parameter
) noexcept {
    return geo::authalic_q(latitude0 + parameter * latitude_delta);
}

[[nodiscard]] double simpson(
    const double a,
    const double b,
    const double fa,
    const double fm,
    const double fb
) noexcept {
    return (b - a) * (fa + 4.0 * fm + fb) / 6.0;
}

[[nodiscard]] IntegralResult integrate_q_recursive(
    const double latitude0,
    const double latitude_delta,
    const double a,
    const double b,
    const double fa,
    const double fm,
    const double fb,
    const double whole,
    const double tolerance,
    const unsigned depth,
    const unsigned max_depth
) noexcept {
    IntegralResult result{};
    result.deepest_level = depth;

    const double midpoint = a + (b - a) * 0.5;
    const double left_midpoint = a + (midpoint - a) * 0.5;
    const double right_midpoint = midpoint + (b - midpoint) * 0.5;

    const double f_left_midpoint =
        q_on_linear_latitude(latitude0, latitude_delta, left_midpoint);
    const double f_right_midpoint =
        q_on_linear_latitude(latitude0, latitude_delta, right_midpoint);

    if (!std::isfinite(f_left_midpoint) || !std::isfinite(f_right_midpoint)) {
        result.error = GeographicError::numerical_domain_error;
        return result;
    }

    const double left = simpson(a, midpoint, fa, f_left_midpoint, fm);
    const double right = simpson(midpoint, b, fm, f_right_midpoint, fb);
    const double refined = left + right;
    const double delta = refined - whole;

    if (!std::isfinite(refined) || !std::isfinite(delta)) {
        result.error = GeographicError::numerical_domain_error;
        return result;
    }

    if (std::abs(delta) <= 15.0 * tolerance) {
        result.value = refined + delta / 15.0;
        result.estimated_abs_error = std::abs(delta) / 15.0;
        return result;
    }

    if (depth >= max_depth) {
        result.error = GeographicError::integration_limit_exceeded;
        return result;
    }

    const IntegralResult left_result = integrate_q_recursive(
        latitude0,
        latitude_delta,
        a,
        midpoint,
        fa,
        f_left_midpoint,
        fm,
        left,
        tolerance * 0.5,
        depth + 1U,
        max_depth
    );
    if (!left_result.ok()) {
        return left_result;
    }

    const IntegralResult right_result = integrate_q_recursive(
        latitude0,
        latitude_delta,
        midpoint,
        b,
        fm,
        f_right_midpoint,
        fb,
        right,
        tolerance * 0.5,
        depth + 1U,
        max_depth
    );
    if (!right_result.ok()) {
        return right_result;
    }

    result.value = left_result.value + right_result.value;
    result.estimated_abs_error =
        left_result.estimated_abs_error + right_result.estimated_abs_error;
    result.deepest_level =
        std::max(left_result.deepest_level, right_result.deepest_level);
    return result;
}

[[nodiscard]] IntegralResult integrate_q_linear_edge(
    const double latitude0,
    const double latitude1,
    const GeographicAreaOptions& options
) noexcept {
    IntegralResult result{};

    if (latitude0 == latitude1) {
        const double value = geo::authalic_q(latitude0);
        if (!std::isfinite(value)) {
            result.error = GeographicError::numerical_domain_error;
            return result;
        }
        result.value = value;
        return result;
    }

    const double latitude_delta = latitude1 - latitude0;
    const double fa = geo::authalic_q(latitude0);
    const double fm = geo::authalic_q(latitude0 + latitude_delta * 0.5);
    const double fb = geo::authalic_q(latitude1);
    if (!std::isfinite(fa) || !std::isfinite(fm) || !std::isfinite(fb)) {
        result.error = GeographicError::numerical_domain_error;
        return result;
    }

    const double whole = simpson(0.0, 1.0, fa, fm, fb);
    return integrate_q_recursive(
        latitude0,
        latitude_delta,
        0.0,
        1.0,
        fa,
        fm,
        fb,
        whole,
        options.edge_integral_tolerance,
        0U,
        options.max_integration_depth
    );
}

[[nodiscard]] double wgs84_surface_area_m2() noexcept {
    const double a = geo::Wgs84::semi_major_axis_m;
    return 2.0 * geo::kPi * a * a * geo::authalic_q_pole();
}

[[nodiscard]] bool select_topological_area(
    const double raw_area_m2,
    const int longitude_winding,
    const RingInteriorSide interior_side,
    double& selected_area_m2,
    double& topology_roundoff_m2
) noexcept {
    const double surface_area = wgs84_surface_area_m2();
    if (!std::isfinite(surface_area) || surface_area <= 0.0) {
        return false;
    }

    const double winding = static_cast<double>(longitude_winding);
    const double branch_area = raw_area_m2 + winding * (0.5 * surface_area);
    if (!std::isfinite(branch_area)) {
        return false;
    }

    double representative = std::fmod(branch_area, surface_area);
    if (!std::isfinite(representative)) {
        return false;
    }

    if (interior_side == RingInteriorSide::left) {
        if (representative < 0.0) {
            representative += surface_area;
        }
    } else if (interior_side == RingInteriorSide::right) {
        if (representative > 0.0) {
            representative -= surface_area;
        }
    } else {
        return false;
    }

    if (!std::isfinite(representative)) {
        return false;
    }

    selected_area_m2 = representative;
    topology_roundoff_m2 =
        16.0 * std::numeric_limits<double>::epsilon() *
        (std::abs(raw_area_m2) +
         std::abs(winding) * 0.5 * surface_area +
         surface_area);
    return std::isfinite(topology_roundoff_m2);
}

}  // namespace

LinearRingResult canonicalize_wgs84_linear_ring(
    const GeodeticPoint* const points,
    const std::size_t count
) {
    LinearRingResult result{};

    if (points == nullptr || count < 3U) {
        result.error = GeographicError::too_few_vertices;
        return result;
    }

    for (std::size_t index = 0U; index < count; ++index) {
        if (!std::isfinite(points[index].longitude_rad) ||
            !std::isfinite(points[index].latitude_rad)) {
            result.error = GeographicError::non_finite_coordinate;
            return result;
        }
        if (points[index].latitude_rad < -geo::kHalfPi ||
            points[index].latitude_rad > geo::kHalfPi) {
            result.error = GeographicError::latitude_out_of_range;
            return result;
        }
    }

    std::size_t effective_count = count;
    if (count >= 4U &&
        points[count - 1U].latitude_rad == points[0].latitude_rad &&
        std::remainder(points[count - 1U].longitude_rad - points[0].longitude_rad, kTwoPi) == 0.0) {
        --effective_count;
    }

    if (effective_count < 3U) {
        result.error = GeographicError::too_few_vertices;
        return result;
    }

    result.value.vertices.reserve(effective_count);

    double previous_raw_longitude = points[0].longitude_rad;
    double current_unwrapped_longitude = principal_longitude(previous_raw_longitude);
    result.value.vertices.push_back({current_unwrapped_longitude, points[0].latitude_rad});

    for (std::size_t index = 1U; index < effective_count; ++index) {
        const double raw_longitude = points[index].longitude_rad;
        const double delta = std::remainder(raw_longitude - previous_raw_longitude, kTwoPi);
        if (ambiguous_half_turn(delta)) {
            result.error = GeographicError::ambiguous_half_turn;
            result.value = {};
            return result;
        }

        current_unwrapped_longitude += delta;
        result.value.vertices.push_back({
            current_unwrapped_longitude,
            points[index].latitude_rad,
        });
        previous_raw_longitude = raw_longitude;
    }

    const double closing_delta = std::remainder(
        points[0].longitude_rad - points[effective_count - 1U].longitude_rad,
        kTwoPi
    );
    if (ambiguous_half_turn(closing_delta)) {
        result.error = GeographicError::ambiguous_half_turn;
        result.value = {};
        return result;
    }

    result.value.closing_longitude_rad = current_unwrapped_longitude + closing_delta;

    const double turns =
        (result.value.closing_longitude_rad - result.value.vertices.front().longitude_rad) /
        kTwoPi;
    const long long rounded_turns = std::llround(turns);
    constexpr double winding_tolerance = 256.0 * std::numeric_limits<double>::epsilon();
    if (!std::isfinite(turns) ||
        std::abs(turns - static_cast<double>(rounded_turns)) > winding_tolerance ||
        rounded_turns < static_cast<long long>(std::numeric_limits<int>::min()) ||
        rounded_turns > static_cast<long long>(std::numeric_limits<int>::max())) {
        result.error = GeographicError::numerical_domain_error;
        result.value = {};
        return result;
    }

    result.value.longitude_winding = static_cast<int>(rounded_turns);
    return result;
}

GeodeticPoint interpolate_wgs84_linear_edge(
    const GeodeticPoint start,
    const GeodeticPoint end,
    const double parameter
) noexcept {
    return {
        start.longitude_rad + parameter * (end.longitude_rad - start.longitude_rad),
        start.latitude_rad + parameter * (end.latitude_rad - start.latitude_rad),
    };
}

GeographicAreaResult signed_wgs84_linear_ring_area(
    const LinearRing& ring,
    const GeographicAreaOptions& options
) noexcept {
    GeographicAreaResult result{};

    if (ring.vertices.size() < 3U) {
        result.error = GeographicError::too_few_vertices;
        return result;
    }
    if (!std::isfinite(options.edge_integral_tolerance) ||
        options.edge_integral_tolerance <= 0.0 ||
        options.max_integration_depth == 0U) {
        result.error = GeographicError::invalid_options;
        return result;
    }
    if (ring.longitude_winding != 0 &&
        ring.interior_side == RingInteriorSide::unspecified) {
        result.error = GeographicError::longitude_winding_unsupported;
        return result;
    }

    for (const GeodeticPoint point : ring.vertices) {
        if (!valid_point(point)) {
            result.error = std::isfinite(point.longitude_rad) && std::isfinite(point.latitude_rad)
                ? GeographicError::latitude_out_of_range
                : GeographicError::non_finite_coordinate;
            return result;
        }
    }
    if (!std::isfinite(ring.closing_longitude_rad)) {
        result.error = GeographicError::non_finite_coordinate;
        return result;
    }

    const double scale = -0.5 * geo::Wgs84::semi_major_axis_m * geo::Wgs84::semi_major_axis_m;
    double area_sum = 0.0;
    double area_compensation = 0.0;
    double error_sum = 0.0;

    for (std::size_t index = 0U; index < ring.vertices.size(); ++index) {
        const GeodeticPoint start = ring.vertices[index];
        const GeodeticPoint end = index + 1U < ring.vertices.size()
            ? ring.vertices[index + 1U]
            : GeodeticPoint{
                  ring.closing_longitude_rad,
                  ring.vertices.front().latitude_rad,
              };

        const double longitude_delta = end.longitude_rad - start.longitude_rad;
        if (longitude_delta == 0.0) {
            continue;
        }

        const IntegralResult integral = integrate_q_linear_edge(
            start.latitude_rad,
            end.latitude_rad,
            options
        );
        if (!integral.ok()) {
            result.error = integral.error;
            return result;
        }

        result.deepest_integration_level =
            std::max(result.deepest_integration_level, integral.deepest_level);

        const double contribution = scale * longitude_delta * integral.value;
        const double contribution_error =
            std::abs(scale * longitude_delta) * integral.estimated_abs_error;
        if (!std::isfinite(contribution) || !std::isfinite(contribution_error)) {
            result.error = GeographicError::numerical_domain_error;
            return result;
        }

        static_cast<void>(compensated_add(
            contribution,
            area_sum,
            area_compensation
        ));
        error_sum += contribution_error;
    }

    if (!std::isfinite(area_sum) || !std::isfinite(error_sum)) {
        result.error = GeographicError::numerical_domain_error;
        return result;
    }

    if (ring.interior_side != RingInteriorSide::unspecified) {
        double selected_area = 0.0;
        double topology_roundoff = 0.0;
        if (!select_topological_area(
                area_sum,
                ring.longitude_winding,
                ring.interior_side,
                selected_area,
                topology_roundoff
            )) {
            result.error = GeographicError::numerical_domain_error;
            return result;
        }
        area_sum = selected_area;
        error_sum += topology_roundoff;
    }

    if (!std::isfinite(area_sum) || !std::isfinite(error_sum)) {
        result.error = GeographicError::numerical_domain_error;
        return result;
    }

    result.signed_area_m2 = area_sum;
    result.estimated_abs_error_m2 = error_sum;
    return result;
}

}  // namespace aeris::geometry
