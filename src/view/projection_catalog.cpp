// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/view/projection_catalog.hpp"

namespace aeris::view {
namespace {

constexpr ProjectionCatalog kCatalog{{
    {
        SurfaceMode::sinu_mollweide,
        "sinu-mollweide",
        "Sinu-Mollweide",
        {true, true, true},
    },
    {
        SurfaceMode::mollweide,
        "mollweide",
        "Mollweide",
        {true, true, true},
    },
    {
        SurfaceMode::sinusoidal,
        "sinusoidal",
        "Sinusoidal",
        {true, true, true},
    },
}};

}  // namespace

const ProjectionCatalog& projection_catalog() noexcept {
    return kCatalog;
}

const ProjectionDescriptor* find_projection_descriptor(
    const SurfaceMode mode
) noexcept {
    for (const auto& descriptor : kCatalog) {
        if (descriptor.mode == mode) return &descriptor;
    }
    return nullptr;
}

}  // namespace aeris::view
