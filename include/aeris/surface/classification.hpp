// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace aeris::surface {

enum class SurfaceClass : std::uint8_t {
    unknown = 0U,
    water,
    land,
    grounded_ice,
    floating_ice_shelf,
};

// Canonical feature-property channel used by verified vector classification
// sources. The property value is one of the stable generation-1 identifiers
// returned by surface_class_id().
inline constexpr std::string_view kSurfaceClassPropertyKey =
    "aeris.surface_class.v1";

[[nodiscard]] constexpr std::string_view surface_class_id(
    const SurfaceClass value
) noexcept {
    switch (value) {
        case SurfaceClass::unknown:
            return "unknown";
        case SurfaceClass::water:
            return "water";
        case SurfaceClass::land:
            return "land";
        case SurfaceClass::grounded_ice:
            return "grounded_ice";
        case SurfaceClass::floating_ice_shelf:
            return "floating_ice_shelf";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::optional<SurfaceClass> parse_surface_class_id(
    const std::string_view value
) noexcept {
    if (value == "unknown") return SurfaceClass::unknown;
    if (value == "water") return SurfaceClass::water;
    if (value == "land") return SurfaceClass::land;
    if (value == "grounded_ice") return SurfaceClass::grounded_ice;
    if (value == "floating_ice_shelf") return SurfaceClass::floating_ice_shelf;
    return std::nullopt;
}

}  // namespace aeris::surface
