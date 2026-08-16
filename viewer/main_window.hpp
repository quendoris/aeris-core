// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "aeris/source/adapter.hpp"
#include "scene_controller.hpp"
#include "unfold_controller.hpp"

#include <QElapsedTimer>
#include <QMainWindow>
#include <QTimer>

#include <memory>

class QAction;
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

    void present_scene(SceneData scene);
    void present_unfold_frame(UnfoldBundle bundle, double progress);

private:
    void set_mode(ViewMode mode);
    void request_current_verified();
    void start_unfold();
    void accept_unfold_bundle(UnfoldBundle bundle);
    void advance_unfold();
    void cancel_unfold_activity();
    void finish_unfold_animation();
    void update_inspector(const SceneData& scene);
    void apply_scene_busy(bool busy);
    void apply_unfold_busy(bool busy);
    void refresh_interaction_state();
    void update_unfold_action();
    void set_view_actions_enabled(bool enabled);
    void select_mode_action(ViewMode mode);
    void build_workbench();
    void apply_theme();

    std::shared_ptr<const source::Result> world_;
    SceneController controller_;
    UnfoldController unfold_controller_;
    MapCanvas* canvas_ = nullptr;
    QLabel* source_value_ = nullptr;
    QLabel* mode_value_ = nullptr;
    QLabel* camera_value_ = nullptr;
    QLabel* geometry_value_ = nullptr;
    QLabel* state_value_ = nullptr;
    QAction* globe_action_ = nullptr;
    QAction* sinusoidal_action_ = nullptr;
    QAction* mollweide_action_ = nullptr;
    QAction* unfold_action_ = nullptr;

    QTimer unfold_timer_;
    QElapsedTimer unfold_clock_;
    ViewMode mode_ = ViewMode::globe;
    ViewMode last_flat_mode_ = ViewMode::mollweide;
    ViewMode unfold_target_ = ViewMode::mollweide;
    double longitude_deg_ = 15.0;
    double latitude_deg_ = 20.0;
    bool scene_busy_ = false;
    bool unfold_preparing_ = false;
    bool unfold_animating_ = false;
    bool scene_verified_ = false;
};

}  // namespace aeris::viewer
