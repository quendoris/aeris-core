// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/geo/wgs84.hpp"

#include <cmath>
#include <limits>

namespace aeris::geo {
namespace {

[[nodiscard]] bool latitude_in_domain(const double latitude_rad) noexcept {
    return std::isfinite(latitude_rad) && latitude_rad >= -kHalfPi && latitude_rad <= kHalfPi;
}

[[nodiscard]] double clamp_roundoff_unit_interval(const double value, bool& valid) noexcept {
    constexpr double slack = 16.0 * std::numeric_limits<double>::epsilon();

    if (value > 1.0) {
        if (value <= 1.0 + slack) {
            return 1.0;
        }
        valid = false;
        return value;
    }

    if (value < -1.0) {
        if (value >= -1.0 - slack) {
            return -1.0;
        }
        valid = false;
        return value;
    }

    return value;
}

[[nodiscard]] double midpoint(const double lower, const double upper) noexcept {
    return lower + (upper - lower) * 0.5;
}

}  // namespace

double wgs84_eccentricity() noexcept {
    return std::sqrt(Wgs84::eccentricity_squared);
}

double authalic_q(const double geodetic_latitude_rad) noexcept {
    if (!latitude_in_domain(geodetic_latitude_rad)) {
        return std::numeric_limits<double>::quiet_NaN();
    }

    const double e = wgs84_eccentricity();
    const double sin_phi = std::sin(geodetic_latitude_rad);
    const double e_sin_phi = e * sin_phi;
    const double denominator = 1.0 - Wgs84::eccentricity_squared * sin_phi * sin_phi;
    const double inverse_hyperbolic = std::atanh(e_sin_phi);

    return (1.0 - Wgs84::eccentricity_squared) *
           ((sin_phi / denominator) + (inverse_hyperbolic / e));
}

double authalic_q_pole() noexcept {
    return authalic_q(kHalfPi);
}

double authalic_radius_m() noexcept {
    return Wgs84::semi_major_axis_m * std::sqrt(authalic_q_pole() / 2.0);
}

ScalarResult authalic_latitude(const double geodetic_latitude_rad) noexcept {
    if (!std::isfinite(geodetic_latitude_rad)) {
        return {0.0, MathError::non_finite_input};
    }

    if (geodetic_latitude_rad < -kHalfPi || geodetic_latitude_rad > kHalfPi) {
        return {0.0, MathError::latitude_out_of_range};
    }

    if (geodetic_latitude_rad == kHalfPi) {
        return {kHalfPi, MathError::none};
    }
    if (geodetic_latitude_rad == -kHalfPi) {
        return {-kHalfPi, MathError::none};
    }

    const double q_pole = authalic_q_pole();
    const double q = authalic_q(geodetic_latitude_rad);
    if (!std::isfinite(q) || !std::isfinite(q_pole) || q_pole <= 0.0) {
        return {0.0, MathError::numerical_domain_error};
    }

    bool valid = true;
    const double ratio = clamp_roundoff_unit_interval(q / q_pole, valid);
    if (!valid) {
        return {0.0, MathError::numerical_domain_error};
    }

    const double beta = std::asin(ratio);
    if (!std::isfinite(beta)) {
        return {0.0, MathError::numerical_domain_error};
    }

    return {beta, MathError::none};
}

ScalarResult geodetic_latitude_from_authalic(const double authalic_latitude_rad) noexcept {
    if (!std::isfinite(authalic_latitude_rad)) {
        return {0.0, MathError::non_finite_input};
    }

    if (authalic_latitude_rad < -kHalfPi || authalic_latitude_rad > kHalfPi) {
        return {0.0, MathError::latitude_out_of_range};
    }

    if (authalic_latitude_rad == kHalfPi) {
        return {kHalfPi, MathError::none};
    }
    if (authalic_latitude_rad == -kHalfPi) {
        return {-kHalfPi, MathError::none};
    }
    if (authalic_latitude_rad == 0.0) {
        return {0.0, MathError::none};
    }

    const double q_pole = authalic_q_pole();
    const double target_q = q_pole * std::sin(authalic_latitude_rad);
    if (!std::isfinite(q_pole) || q_pole <= 0.0 || !std::isfinite(target_q)) {
        return {0.0, MathError::numerical_domain_error};
    }

    // Coordinate-bounded bisection is deliberately used for the reference inverse.
    // It is slower than Newton iteration, but its stopping condition is expressed
    // directly in the recovered latitude rather than in a libm-dependent residual.
    // This keeps the CPU reference path portable across standard-library implementations.
    double lower = -kHalfPi;
    double upper = kHalfPi;

    // q(phi) is monotone on the complete geodetic latitude domain. Therefore every
    // finite comparison below preserves a valid bracket. Eighty bisections contract
    // the original pi-radian interval to roughly 2.6e-24 rad, far below the useful
    // binary64 angular resolution even for authalic latitudes extremely close to
    // zero. Requiring midpoint == endpoint after that contraction is incorrect:
    // tiny valid targets can retain two distinct subnormal-scale bracket endpoints
    // and were previously reported as non-convergent despite an already determined
    // solution. The final bracket midpoint is the deterministic bounded solution.
    constexpr int max_iterations = 80;
    for (int iteration = 0; iteration < max_iterations; ++iteration) {
        const double candidate = midpoint(lower, upper);
        if (candidate == lower || candidate == upper) {
            return {candidate, MathError::none};
        }

        const double q_candidate = authalic_q(candidate);
        if (!std::isfinite(q_candidate)) {
            return {0.0, MathError::numerical_domain_error};
        }

        if (q_candidate < target_q) {
            lower = candidate;
        } else if (q_candidate > target_q) {
            upper = candidate;
        } else {
            return {candidate, MathError::none};
        }
    }

    const double candidate = midpoint(lower, upper);
    if (!std::isfinite(candidate)) {
        return {0.0, MathError::numerical_domain_error};
    }
    return {candidate, MathError::none};
}

}  // namespace aeris::geo
