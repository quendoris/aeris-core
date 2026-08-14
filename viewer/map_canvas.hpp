// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "scene.hpp"

#include <QPoint>
#include <QWidget>

#include <functional>

namespace aeris::viewer {

class MapCanvas final : public QWidget {
public:
    using CameraCallback = std::function<void(double, double, bool)>;

    explicit MapCanvas(QWidget* parent = nullptr);

    void set_scene(SceneData scene);
    void set_busy(bool busy);
    void set_camera_callback(CameraCallback callback);
    void set_camera(double longitude_deg, double latitude_deg);

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
    bool busy_ = false;
    bool dragging_ = false;
    QPoint last_mouse_{};
    double longitude_deg_ = 15.0;
    double latitude_deg_ = 20.0;
    double zoom_ = 1.0;
    CameraCallback camera_callback_;
};

}  // namespace aeris::viewer
