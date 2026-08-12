// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/projection/primitives.hpp"

#include <cmath>
#include <limits>

namespace aeris::projection {
namespace {

constexpr double kSqrt2 = 1.414213562373095048801688724209698079;

[[nodiscard]] bool valid_radius(const double radius_m) noexcept {
    return std::isfinite(radius_m) && radius_m > 0.0;
}

[[nodiscard]] bool valid_spherical_input(
    const double longitude_rad,
    const double latitude_rad,
    const double radius_m
) noexcept {
    return std::isfinite(longitude_rad) && std::isfinite(latitude_rad) &&
           latitude_rad >= -geo::kHalfPi && latitude_rad <= geo::kHalfPi &&
           valid_radius(radius_m);
}

[[nodiscard]] double clamp_roundoff_unit(const double value, bool& valid) noexcept {
    constexpr double slack = 32.0 * std::numeric_limits<double>::epsilon();
    if (value > 1.0) {
        if (value <= 1.0 + slack) {
            return 1.0;
        }
        valid = false;
    } else if (value < -1.0) {
        if (value >= -1.0 - slack) {
            return -1.0;
        }
        valid = false;
    }
    return value;
}

}  // namespace

PlanarResult sinusoidal_forward(
    const double longitude_delta_rad,
    const double authalic_latitude_rad,
    const double radius_m
) noexcept {
    if (!valid_spherical_input(longitude_delta_rad, authalic_latitude_rad, radius_m)) {
        return {{}, !std::isfinite(longitude_delta_rad) || !std::isfinite(authalic_latitude_rad) ||
                        !std::isfinite(radius_m)
                    ? geo::MathError::non_finite_input
                    : geo::MathError::latitude_out_of_range};
    }

    return {{
                radius_m * longitude_delta_rad * std::cos(authalic_latitude_rad),
                radius_m * authalic_latitude_rad,
            },
            geo::MathError::none};
}

SphericalResult sinusoidal_inverse(
    const double x,
    const double y,
    const double radius_m
) noexcept {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(radius_m)) {
        return {{}, geo::MathError::non_finite_input};
    }
    if (!valid_radius(radius_m)) {
        return {{}, geo::MathError::numerical_domain_error};
    }

    const double beta = y / radius_m;
    if (beta < -geo::kHalfPi || beta > geo::kHalfPi) {
        return {{}, geo::MathError::latitude_out_of_range};
    }

    const double cos_beta = std::cos(beta);
    constexpr double pole_floor = 64.0 * std::numeric_limits<double>::epsilon();
    if (std::abs(cos_beta) <= pole_floor) {
        if (std::abs(x) <= radius_m * pole_floor) {
            return {{0.0, beta}, geo::MathError::indeterminate_coordinate};
        }
        return {{}, geo::MathError::numerical_domain_error};
    }

    return {{x / (radius_m * cos_beta), beta}, geo::MathError::none};
}

geo::ScalarResult mollweide_auxiliary_angle(const double authalic_latitude_rad) noexcept {
    if (!std::isfinite(authalic_latitude_rad)) {
        return {0.0, geo::MathError::non_finite_input};
    }
    if (authalic_latitude_rad < -geo::kHalfPi || authalic_latitude_rad > geo::kHalfPi) {
        return {0.0, geo::MathError::latitude_out_of_range};
    }
    if (authalic_latitude_rad == geo::kHalfPi) {
        return {geo::kHalfPi, geo::MathError::none};
    }
    if (authalic_latitude_rad == -geo::kHalfPi) {
        return {-geo::kHalfPi, geo::MathError::none};
    }
    if (authalic_latitude_rad == 0.0) {
        return {0.0, geo::MathError::none};
    }

    const double target = geo::kPi * std::sin(authalic_latitude_rad);
    double lower = -geo::kHalfPi;
    double upper = geo::kHalfPi;
    double theta = authalic_latitude_rad;

    constexpr double residual_tolerance = 16.0 * std::numeric_limits<double>::epsilon();
    constexpr double derivative_floor = 128.0 * std::numeric_limits<double>::epsilon();
    constexpr int newton_iterations = 20;

    for (int iteration = 0; iteration < newton_iterations; ++iteration) {
        const double residual = 2.0 * theta + std::sin(2.0 * theta) - target;
        if (std::abs(residual) <= residual_tolerance) {
            return {theta, geo::MathError::none};
        }

        if (residual < 0.0) {
            lower = theta;
        } else {
            upper = theta;
        }

        const double cos_theta = std::cos(theta);
        const double derivative = 4.0 * cos_theta * cos_theta;
        double candidate = 0.5 * (lower + upper);

        if (std::isfinite(derivative) && derivative > derivative_floor) {
            const double newton = theta - residual / derivative;
            if (std::isfinite(newton) && newton > lower && newton < upper) {
                candidate = newton;
            }
        }

        if (candidate == theta) {
            return {theta, geo::MathError::none};
        }
        theta = candidate;
    }

    constexpr int bisection_iterations = 64;
    for (int iteration = 0; iteration < bisection_iterations; ++iteration) {
        const double midpoint = 0.5 * (lower + upper);
        if (midpoint == lower || midpoint == upper) {
            return {midpoint, geo::MathError::none};
        }

        const double residual = 2.0 * midpoint + std::sin(2.0 * midpoint) - target;
        if (std::abs(residual) <= residual_tolerance) {
            return {midpoint, geo::MathError::none};
        }

        if (residual < 0.0) {
            lower = midpoint;
        } else {
            upper = midpoint;
        }
    }

    return {0.0, geo::MathError::non_convergence};
}

PlanarResult mollweide_forward(
    const double longitude_delta_rad,
    const double authalic_latitude_rad,
    const double radius_m
) noexcept {
    if (!valid_spherical_input(longitude_delta_rad, authalic_latitude_rad, radius_m)) {
        return {{}, !std::isfinite(longitude_delta_rad) || !std::isfinite(authalic_latitude_rad) ||
                        !std::isfinite(radius_m)
                    ? geo::MathError::non_finite_input
                    : geo::MathError::latitude_out_of_range};
    }

    const auto theta_result = mollweide_auxiliary_angle(authalic_latitude_rad);
    if (!theta_result.ok()) {
        return {{}, theta_result.error};
    }

    const double theta = theta_result.value;
    return {{
                (2.0 * kSqrt2 / geo::kPi) * radius_m * longitude_delta_rad * std::cos(theta),
                kSqrt2 * radius_m * std::sin(theta),
            },
            geo::MathError::none};
}

SphericalResult mollweide_inverse(
    const double x,
    const double y,
    const double radius_m
) noexcept {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(radius_m)) {
        return {{}, geo::MathError::non_finite_input};
    }
    if (!valid_radius(radius_m)) {
        return {{}, geo::MathError::numerical_domain_error};
    }

    bool valid = true;
    const double y_ratio = clamp_roundoff_unit(y / (kSqrt2 * radius_m), valid);
    if (!valid) {
        return {{}, geo::MathError::latitude_out_of_range};
    }

    const double theta = std::asin(y_ratio);
    const double sin_beta = clamp_roundoff_unit(
        (2.0 * theta + std::sin(2.0 * theta)) / geo::kPi,
        valid
    );
    if (!valid) {
        return {{}, geo::MathError::numerical_domain_error};
    }

    const double beta = std::asin(sin_beta);
    const double cos_theta = std::cos(theta);
    constexpr double pole_floor = 64.0 * std::numeric_limits<double>::epsilon();
    if (std::abs(cos_theta) <= pole_floor) {
        if (std::abs(x) <= radius_m * pole_floor) {
            return {{0.0, beta}, geo::MathError::indeterminate_coordinate};
        }
        return {{}, geo::MathError::numerical_domain_error};
    }

    const double longitude_delta =
        (geo::kPi * x) / (2.0 * kSqrt2 * radius_m * cos_theta);
    if (!std::isfinite(longitude_delta)) {
        return {{}, geo::MathError::numerical_domain_error};
    }

    return {{longitude_delta, beta}, geo::MathError::none};
}

}  // namespace aeris::projection
