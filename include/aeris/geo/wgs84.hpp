// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

namespace aeris::geo {

inline constexpr double kPi = 3.141592653589793238462643383279502884;
inline constexpr double kHalfPi = kPi / 2.0;

struct Wgs84 final {
    static constexpr double semi_major_axis_m = 6378137.0;
    static constexpr double inverse_flattening = 298.257223563;
    static constexpr double flattening = 1.0 / inverse_flattening;
    static constexpr double eccentricity_squared = flattening * (2.0 - flattening);
};

enum class MathError {
    none = 0,
    non_finite_input,
    latitude_out_of_range,
    numerical_domain_error,
};

struct ScalarResult final {
    double value = 0.0;
    MathError error = MathError::none;

    [[nodiscard]] constexpr bool ok() const noexcept {
        return error == MathError::none;
    }
};

[[nodiscard]] double wgs84_eccentricity() noexcept;
[[nodiscard]] double authalic_q(double geodetic_latitude_rad) noexcept;
[[nodiscard]] double authalic_q_pole() noexcept;
[[nodiscard]] double authalic_radius_m() noexcept;
[[nodiscard]] ScalarResult authalic_latitude(double geodetic_latitude_rad) noexcept;

}  // namespace aeris::geo
