// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/projection/adapter.hpp"

#include "aeris/projection/wgs84.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace aeris::projection {
namespace {

constexpr double kTwoPi = 2.0 * geo::kPi;

[[nodiscard]] bool valid_radius(const double radius_m) noexcept {
    return std::isfinite(radius_m) && radius_m > 0.0;
}

[[nodiscard]] double canonical_longitude(const double value) noexcept {
    double normalized = std::fmod(value, kTwoPi);
    if (normalized <= -geo::kPi) normalized += kTwoPi;
    if (normalized > geo::kPi) normalized -= kTwoPi;
    return normalized == 0.0 ? 0.0 : normalized;
}

[[nodiscard]] GeodeticResult inverse_authalic_result(
    const SphericalResult& spherical,
    const double central_meridian_rad
) noexcept {
    if (!spherical.ok()) {
        return {{}, spherical.error};
    }
    if (!std::isfinite(central_meridian_rad)) {
        return {{}, geo::MathError::non_finite_input};
    }

    const geo::ScalarResult latitude =
        geo::geodetic_latitude_from_authalic(spherical.value.latitude_rad);
    if (!latitude.ok()) {
        return {{}, latitude.error};
    }

    return {{
                canonical_longitude(
                    central_meridian_rad + spherical.value.longitude_rad
                ),
                latitude.value,
            },
            geo::MathError::none};
}

class SinusoidalProjectionAdapter final : public ProjectionAdapter {
public:
    [[nodiscard]] ProjectionDescriptor descriptor() const noexcept override {
        return {
            kProjectionModelSinusoidalV1,
            "Sinusoidal",
            kProjectionCutSingleAntimeridianV1,
            ProjectionAreaContract::equal_area,
            ProjectionCutTopology::single_antimeridian,
        };
    }

    [[nodiscard]] PlanarResult forward_wgs84(
        const double longitude_rad,
        const double geodetic_latitude_rad,
        const double central_meridian_rad,
        const double radius_m
    ) const noexcept override {
        return project_wgs84_primitive(
            longitude_rad,
            geodetic_latitude_rad,
            EqualAreaPrimitive::sinusoidal,
            central_meridian_rad,
            radius_m
        );
    }

    [[nodiscard]] GeodeticResult inverse_wgs84(
        const double x,
        const double y,
        const double central_meridian_rad,
        const double radius_m
    ) const noexcept override {
        return inverse_authalic_result(
            sinusoidal_inverse(x, y, radius_m),
            central_meridian_rad
        );
    }
};

class MollweideProjectionAdapter final : public ProjectionAdapter {
public:
    [[nodiscard]] ProjectionDescriptor descriptor() const noexcept override {
        return {
            kProjectionModelMollweideV1,
            "Mollweide",
            kProjectionCutSingleAntimeridianV1,
            ProjectionAreaContract::equal_area,
            ProjectionCutTopology::single_antimeridian,
        };
    }

    [[nodiscard]] PlanarResult forward_wgs84(
        const double longitude_rad,
        const double geodetic_latitude_rad,
        const double central_meridian_rad,
        const double radius_m
    ) const noexcept override {
        return project_wgs84_primitive(
            longitude_rad,
            geodetic_latitude_rad,
            EqualAreaPrimitive::mollweide,
            central_meridian_rad,
            radius_m
        );
    }

    [[nodiscard]] GeodeticResult inverse_wgs84(
        const double x,
        const double y,
        const double central_meridian_rad,
        const double radius_m
    ) const noexcept override {
        return inverse_authalic_result(
            mollweide_inverse(x, y, radius_m),
            central_meridian_rad
        );
    }
};

class LambertCylindricalEqualAreaProjectionAdapter final : public ProjectionAdapter {
public:
    [[nodiscard]] ProjectionDescriptor descriptor() const noexcept override {
        return {
            kProjectionModelLambertCylindricalEqualAreaV1,
            "Lambert Cylindrical Equal-Area",
            kProjectionCutSingleAntimeridianV1,
            ProjectionAreaContract::equal_area,
            ProjectionCutTopology::single_antimeridian,
        };
    }

    [[nodiscard]] PlanarResult forward_wgs84(
        const double longitude_rad,
        const double geodetic_latitude_rad,
        const double central_meridian_rad,
        const double radius_m
    ) const noexcept override {
        if (!std::isfinite(longitude_rad) ||
            !std::isfinite(central_meridian_rad) ||
            !valid_radius(radius_m)) {
            return {{},
                    !std::isfinite(longitude_rad) ||
                            !std::isfinite(central_meridian_rad) ||
                            !std::isfinite(radius_m)
                        ? geo::MathError::non_finite_input
                        : geo::MathError::numerical_domain_error};
        }

        const geo::ScalarResult beta =
            geo::authalic_latitude(geodetic_latitude_rad);
        if (!beta.ok()) {
            return {{}, beta.error};
        }

        return {{
                    radius_m * (longitude_rad - central_meridian_rad),
                    radius_m * std::sin(beta.value),
                },
                geo::MathError::none};
    }

    [[nodiscard]] GeodeticResult inverse_wgs84(
        const double x,
        const double y,
        const double central_meridian_rad,
        const double radius_m
    ) const noexcept override {
        if (!std::isfinite(x) || !std::isfinite(y) ||
            !std::isfinite(central_meridian_rad) ||
            !std::isfinite(radius_m)) {
            return {{}, geo::MathError::non_finite_input};
        }
        if (!valid_radius(radius_m)) {
            return {{}, geo::MathError::numerical_domain_error};
        }

        constexpr double slack = 64.0 * std::numeric_limits<double>::epsilon();
        double y_ratio = y / radius_m;
        if (y_ratio < -1.0 - slack || y_ratio > 1.0 + slack) {
            return {{}, geo::MathError::latitude_out_of_range};
        }
        y_ratio = std::max(-1.0, std::min(1.0, y_ratio));

        const double longitude_delta = x / radius_m;
        if (!std::isfinite(longitude_delta) ||
            longitude_delta < -geo::kPi - slack ||
            longitude_delta > geo::kPi + slack) {
            return {{}, geo::MathError::numerical_domain_error};
        }

        const double beta = std::asin(y_ratio);
        const geo::ScalarResult latitude =
            geo::geodetic_latitude_from_authalic(beta);
        if (!latitude.ok()) {
            return {{}, latitude.error};
        }

        return {{
                    canonical_longitude(central_meridian_rad + longitude_delta),
                    latitude.value,
                },
                geo::MathError::none};
    }
};

}  // namespace

const ProjectionAdapter& sinusoidal_projection_adapter() noexcept {
    static const SinusoidalProjectionAdapter adapter{};
    return adapter;
}

const ProjectionAdapter& mollweide_projection_adapter() noexcept {
    static const MollweideProjectionAdapter adapter{};
    return adapter;
}

const ProjectionAdapter& lambert_cylindrical_equal_area_projection_adapter() noexcept {
    static const LambertCylindricalEqualAreaProjectionAdapter adapter{};
    return adapter;
}

BuiltinProjectionAdapters builtin_projection_adapters() noexcept {
    return {{
        &sinusoidal_projection_adapter(),
        &mollweide_projection_adapter(),
        &lambert_cylindrical_equal_area_projection_adapter(),
    }};
}

const ProjectionAdapter* find_builtin_projection_adapter(
    const std::string_view model_id
) noexcept {
    for (const ProjectionAdapter* const adapter : builtin_projection_adapters()) {
        if (adapter != nullptr && adapter->descriptor().model_id == model_id) {
            return adapter;
        }
    }
    return nullptr;
}

}  // namespace aeris::projection
