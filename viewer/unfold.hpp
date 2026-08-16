// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "scene_builder.hpp"

#include <vector>

namespace aeris::viewer {

enum class UnfoldGuideKind {
    graticule = 0,
    seam,
};

struct UnfoldGuideVertex final {
    geometry::PlanarPoint globe{};
    geometry::PlanarPoint flat{};
    double globe_depth_normalized = 0.0;
};

struct UnfoldGuideLine final {
    UnfoldGuideKind kind = UnfoldGuideKind::graticule;
    std::vector<UnfoldGuideVertex> vertices;
};

struct UnfoldBundle final {
    SceneData globe_endpoint{};
    SceneData flat_endpoint{};
    ViewMode target_mode = ViewMode::mollweide;
    std::vector<UnfoldGuideLine> guides;

    bool canceled = false;
    bool ok = true;
    std::string diagnostic;
};

[[nodiscard]] double unfold_eased_progress(double progress) noexcept;

[[nodiscard]] geometry::PlanarPoint interpolate_unfold_vertex(
    const UnfoldGuideVertex& vertex,
    double progress
) noexcept;

// Visibility weight for the explanatory guide only. Geometry that starts on
// the visible hemisphere is fully visible at p=0; geometry that starts behind
// the globe is introduced progressively and is fully visible at p=1.
[[nodiscard]] double unfold_guide_visibility(
    const UnfoldGuideVertex& vertex,
    double progress
) noexcept;

// Build independently verified endpoint scenes plus a deterministic geographic
// guide used only for explanatory animation. Intermediate guide geometry is
// explicitly non-normative and must never be used for export or area claims.
[[nodiscard]] UnfoldBundle build_unfold_bundle(
    const source::Result& world,
    double camera_longitude_deg,
    double camera_latitude_deg,
    ViewMode target_mode,
    const CancelCheck& canceled = {}
);

}  // namespace aeris::viewer
