// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "layer_stack.hpp"
#include "scene.hpp"
#include "unfold.hpp"

#include <QPoint>
#include <QWidget>

#include <functional>
#include <optional>

namespace aeris::viewer {

class MapCanvas final : public QWidget {
public:
    using CameraCallback = std::function<void(double, double, bool)>;

    explicit MapCanvas(QWidget* parent = nullptr);

    void set_scene(SceneData scene);
    void set_busy(bool busy);
    void set_layer_render_state(LayerRenderState state);
    void set_camera_callback(CameraCallback callback);
    void set_camera(double longitude_deg, double latitude_deg);

    void begin_unfold(UnfoldBundle bundle);
    void set_unfold_progress(double progress);
    [[nodiscard]] const SceneData& finish_unfold();
    void cancel_unfold();
    [[nodiscard]] bool is_unfolding() const noexcept { return unfold_.has_value(); }

    [[nodiscard]] const SceneData& scene() const noexcept { return scene_; }

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    void emit_camera(bool final);

    SceneData scene_{};
    std::optional<UnfoldBundle> unfold_;
    LayerRenderState layer_render_state_{};
    double unfold_progress_ = 0.0;
    bool busy_ = false;
    bool dragging_ = false;
    QPoint last_mouse_{};
    double longitude_deg_ = 15.0;
    double latitude_deg_ = 20.0;
    double zoom_ = 1.0;
    CameraCallback camera_callback_;
};

}  // namespace aeris::viewer
