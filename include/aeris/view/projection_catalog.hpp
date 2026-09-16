// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "aeris/view/scene.hpp"

#include <array>
#include <cstddef>

namespace aeris::view {

// Capabilities describe contracts that are already implemented and tested for
// a selectable planar projection surface. They are intentionally descriptive:
// frontends may use them to enable tools, but they do not own projection math.
struct ProjectionCapabilities final {
    bool equal_area = false;
    bool inverse_mapping = false;
    bool interactive_cut = false;
};

// One user-selectable planar surface. SurfaceMode remains the render-neutral
// execution identity so callers do not need a second enum or a conversion
// table merely to enumerate supported projections.
struct ProjectionDescriptor final {
    SurfaceMode mode = SurfaceMode::sinu_mollweide;
    const char* stable_id = "";
    const char* display_name = "";
    ProjectionCapabilities capabilities{};
};

inline constexpr std::size_t kProjectionCatalogSize = 3U;
using ProjectionCatalog = std::array<ProjectionDescriptor, kProjectionCatalogSize>;

// Stable user-facing order for planar projections supported by the current
// core. Globe is deliberately not part of this catalog: it is the folded world
// from which an unfold target is selected, not another planar projection.
[[nodiscard]] const ProjectionCatalog& projection_catalog() noexcept;

// Returns nullptr for Globe and for any future SurfaceMode that has not yet
// been admitted to the supported planar projection catalog.
[[nodiscard]] const ProjectionDescriptor* find_projection_descriptor(
    SurfaceMode mode
) noexcept;

}  // namespace aeris::view
