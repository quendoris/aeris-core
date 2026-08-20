// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "aeris/geometry/geographic.hpp"
#include "aeris/projection/wgs84.hpp"

#include <array>
#include <cstdint>
#include <string_view>

namespace aeris::projection {

inline constexpr std::string_view kProjectionModelSinusoidalV1 =
    "aeris.projection.sinusoidal.v1";
inline constexpr std::string_view kProjectionModelMollweideV1 =
    "aeris.projection.mollweide.v1";
inline constexpr std::string_view kProjectionModelLambertCylindricalEqualAreaV1 =
    "aeris.projection.lambert-cylindrical-equal-area.v1";
inline constexpr std::string_view kProjectionCutSingleAntimeridianV1 =
    "aeris.cut.single-antimeridian.v1";

enum class ProjectionAreaContract : std::uint8_t {
    equal_area = 0U,
};

enum class ProjectionCutTopology : std::uint8_t {
    single_antimeridian = 0U,
};

struct ProjectionDescriptor final {
    std::string_view model_id;
    std::string_view display_name;
    std::string_view cut_model_id;
    ProjectionAreaContract area_contract{ProjectionAreaContract::equal_area};
    ProjectionCutTopology cut_topology{ProjectionCutTopology::single_antimeridian};
};

struct GeodeticResult final {
    geometry::GeodeticPoint value{};
    geo::MathError error = geo::MathError::none;

    [[nodiscard]] constexpr bool ok() const noexcept {
        return error == geo::MathError::none;
    }
};

class ProjectionAdapter {
public:
    virtual ~ProjectionAdapter() = default;

    [[nodiscard]] virtual ProjectionDescriptor descriptor() const noexcept = 0;

    [[nodiscard]] virtual PlanarResult forward_wgs84(
        double longitude_rad,
        double geodetic_latitude_rad,
        double central_meridian_rad = 0.0,
        double radius_m = geo::authalic_radius_m()
    ) const noexcept = 0;

    [[nodiscard]] virtual GeodeticResult inverse_wgs84(
        double x,
        double y,
        double central_meridian_rad = 0.0,
        double radius_m = geo::authalic_radius_m()
    ) const noexcept = 0;
};

[[nodiscard]] const ProjectionAdapter& sinusoidal_projection_adapter() noexcept;
[[nodiscard]] const ProjectionAdapter& mollweide_projection_adapter() noexcept;
[[nodiscard]] const ProjectionAdapter& lambert_cylindrical_equal_area_projection_adapter() noexcept;

// Pre-1.0 compatibility bridge for internal tools that still name one of the
// original mathematical primitives. New project/view code should persist or
// select adapters by model_id and pass ProjectionAdapter directly.
[[nodiscard]] inline const ProjectionAdapter& projection_adapter_for_primitive(
    const EqualAreaPrimitive primitive
) noexcept {
    switch (primitive) {
        case EqualAreaPrimitive::sinusoidal:
            return sinusoidal_projection_adapter();
        case EqualAreaPrimitive::mollweide:
            return mollweide_projection_adapter();
    }
    return sinusoidal_projection_adapter();
}

using BuiltinProjectionAdapters = std::array<const ProjectionAdapter*, 3U>;

[[nodiscard]] BuiltinProjectionAdapters builtin_projection_adapters() noexcept;
[[nodiscard]] const ProjectionAdapter* find_builtin_projection_adapter(
    std::string_view model_id) noexcept;

}  // namespace aeris::projection
