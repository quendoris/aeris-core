// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "aeris/source/adapter.hpp"
#include "scene_controller.hpp"

#include <QMainWindow>

#include <memory>

class QLabel;

namespace aeris::viewer {

class MapCanvas;

class MainWindow final : public QMainWindow {
public:
    explicit MainWindow(
        std::shared_ptr<const source::Result> world,
        bool start_scene = true,
        QWidget* parent = nullptr
    );

    // Present a scene that has already passed the caller's requested scene
    // contract. The normal interactive path uses SceneController; the explicit
    // method also gives deterministic render/screenshot tests the exact same UI
    // presentation boundary without inventing a second test-only canvas.
    void present_scene(SceneData scene);

private:
    void set_mode(ViewMode mode);
    void request_current_verified();
    void update_inspector(const SceneData& scene);
    void apply_busy(bool busy);
    void build_workbench();
    void apply_theme();

    std::shared_ptr<const source::Result> world_;
    SceneController controller_;
    MapCanvas* canvas_ = nullptr;
    QLabel* source_value_ = nullptr;
    QLabel* mode_value_ = nullptr;
    QLabel* camera_value_ = nullptr;
    QLabel* geometry_value_ = nullptr;
    QLabel* state_value_ = nullptr;

    ViewMode mode_ = ViewMode::globe;
    double longitude_deg_ = 15.0;
    double latitude_deg_ = 20.0;
};

}  // namespace aeris::viewer
