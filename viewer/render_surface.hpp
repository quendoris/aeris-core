// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "layer_stack.hpp"
#include "scene.hpp"
#include "unfold.hpp"

#include <QPainter>
#include <QString>

namespace aeris::viewer {

struct CanvasBounds final {
    double min_x = -1.0;
    double min_y = -1.0;
    double max_x = 1.0;
    double max_y = 1.0;
};

[[nodiscard]] CanvasBounds scene_bounds(const SceneData& scene) noexcept;
[[nodiscard]] CanvasBounds interpolate_bounds(
    const CanvasBounds& from,
    const CanvasBounds& to,
    double t
) noexcept;

void apply_world_transform(
    QPainter& painter,
    const CanvasBounds& bounds,
    int width,
    int height,
    double zoom
);

void draw_scene_geometry(
    QPainter& painter,
    const SceneData& scene,
    double opacity,
    const LayerRenderState& layers);
void draw_unfold_guides(QPainter& painter, const UnfoldBundle& bundle, double progress);
[[nodiscard]] QString scene_caption(const SceneData& scene);

}  // namespace aeris::viewer
