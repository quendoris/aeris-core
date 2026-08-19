// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "aeris/geometry/geographic.hpp"
#include "aeris/projection/primitives.hpp"

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

// Projection adapters operate on canonical WGS84 geodetic coordinates and own
// the WGS84 -> authalic-sphere conversion required by AERIS equal-area maps.
// The verified ring pipeline consumes this interface rather than switching on a
// hard-coded projection enum, so adding another equal-area unfolding does not
// require viewer-specific geometry code.
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

    // Returns canonical WGS84 longitude/latitude when the planar coordinate has
    // a unique inverse. Pole singularities remain explicit MathError states.
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

using BuiltinProjectionAdapters = std::array<const ProjectionAdapter*, 3U>;

// Stable catalog order is intentional: frontends may present this list directly
// while project files persist only the versioned model_id.
[[nodiscard]] BuiltinProjectionAdapters builtin_projection_adapters() noexcept;
[[nodiscard]] const ProjectionAdapter* find_builtin_projection_adapter(
    std::string_view model_id) noexcept;

}  // namespace aeris::projection
