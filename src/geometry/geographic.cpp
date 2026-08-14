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

void compensated_add_long_double(
    const long double value,
    long double& sum,
    long double& compensation
) noexcept {
    const long double corrected = value - compensation;
    const long double next = sum + corrected;
    compensation = (next - sum) - corrected;
    sum = next;
}

[[nodiscard]] long double binary64_value_roundoff(const double value) noexcept {
    constexpr long double ulp_budget = 16.0L;
    return ulp_budget * static_cast<long double>(std::numeric_limits<double>::epsilon()) *
           std::max(1.0L, std::abs(static_cast<long double>(value)));
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

[[nodiscard]] bool select_topological_area_dimensionless(
    const long double line_integral,
    const int longitude_winding,
    const RingInteriorSide interior_side,
    long double& selected_dimensionless,
    long double& topology_roundoff_dimensionless
) noexcept {
    const long double pi = static_cast<long double>(geo::kPi);
    const double q_pole_binary64 = geo::authalic_q_pole();
    const long double q_pole = static_cast<long double>(q_pole_binary64);
    if (!std::isfinite(q_pole_binary64) || q_pole <= 0.0L) {
        return false;
    }

    const long double surface_dimensionless = 2.0L * pi * q_pole;
    const long double winding = static_cast<long double>(longitude_winding);
    const long double branch_dimensionless =
        -0.5L * line_integral + winding * pi * q_pole;
    if (!std::isfinite(branch_dimensionless) ||
        !std::isfinite(surface_dimensionless) ||
        surface_dimensionless <= 0.0L) {
        return false;
    }

    long double representative = std::fmod(branch_dimensionless, surface_dimensionless);
    if (!std::isfinite(representative)) {
        return false;
    }

    if (interior_side == RingInteriorSide::left) {
        if (representative < 0.0L) {
            representative += surface_dimensionless;
        }
    } else if (interior_side == RingInteriorSide::right) {
        if (representative > 0.0L) {
            representative -= surface_dimensionless;
        }
    } else {
        return false;
    }

    const long double q_pole_roundoff = binary64_value_roundoff(q_pole_binary64);
    selected_dimensionless = representative;
    topology_roundoff_dimensionless =
        32.0L * std::numeric_limits<long double>::epsilon() *
            (std::abs(line_integral) +
             std::abs(winding) * pi * q_pole +
             surface_dimensionless) +
        std::abs(winding) * pi * q_pole_roundoff +
        2.0L * pi * q_pole_roundoff;
    return std::isfinite(selected_dimensionless) &&
           std::isfinite(topology_roundoff_dimensionless);
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

    long double line_integral = 0.0L;
    long double line_compensation = 0.0L;
    long double dimensionless_error = 0.0L;

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

        const long double contribution =
            static_cast<long double>(longitude_delta) *
            static_cast<long double>(integral.value);
        const long double q_roundoff = binary64_value_roundoff(integral.value);
        const long double contribution_error =
            std::abs(static_cast<long double>(longitude_delta)) *
            (static_cast<long double>(integral.estimated_abs_error) + q_roundoff);
        if (!std::isfinite(contribution) || !std::isfinite(contribution_error)) {
            result.error = GeographicError::numerical_domain_error;
            return result;
        }

        compensated_add_long_double(
            contribution,
            line_integral,
            line_compensation
        );
        dimensionless_error += contribution_error;
    }

    if (!std::isfinite(line_integral) || !std::isfinite(dimensionless_error)) {
        result.error = GeographicError::numerical_domain_error;
        return result;
    }

    long double area_dimensionless = -0.5L * line_integral;
    long double topology_roundoff_dimensionless = 0.0L;
    if (ring.interior_side != RingInteriorSide::unspecified) {
        if (!select_topological_area_dimensionless(
                line_integral,
                ring.longitude_winding,
                ring.interior_side,
                area_dimensionless,
                topology_roundoff_dimensionless
            )) {
            result.error = GeographicError::numerical_domain_error;
            return result;
        }
    }

    const long double a = static_cast<long double>(geo::Wgs84::semi_major_axis_m);
    const long double scale = a * a;
    const long double area_m2 = scale * area_dimensionless;
    const long double error_m2 = scale *
        (0.5L * dimensionless_error + topology_roundoff_dimensionless);
    if (!std::isfinite(area_m2) || !std::isfinite(error_m2)) {
        result.error = GeographicError::numerical_domain_error;
        return result;
    }

    result.signed_area_m2 = static_cast<double>(area_m2);
    result.estimated_abs_error_m2 = static_cast<double>(error_m2);
    if (!std::isfinite(result.signed_area_m2) ||
        !std::isfinite(result.estimated_abs_error_m2)) {
        result.error = GeographicError::numerical_domain_error;
    }
    return result;
}

}  // namespace aeris::geometry
