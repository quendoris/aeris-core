// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "aeris/source/adapter.hpp"
#include "layer_stack.hpp"
#include "scene_controller.hpp"
#include "unfold_controller.hpp"
#include "world_loader.hpp"

#include <QElapsedTimer>
#include <QMainWindow>
#include <QTimer>

#include <memory>

class QAction;
class QLabel;
class QTreeWidget;

namespace aeris::viewer {

class MapCanvas;

class MainWindow final : public QMainWindow {
public:
    explicit MainWindow(
        std::shared_ptr<const source::Result> physical_world,
        std::shared_ptr<const source::Result> political_world,
        MapContent initial_content = MapContent::physical,
        bool start_scene = true,
        QWidget* parent = nullptr
    );

    void present_scene(SceneData scene);
    void present_unfold_frame(UnfoldBundle bundle, double progress);

private:
    void set_mode(ViewMode mode);
    void set_content(MapContent content);
    void request_current_verified();
    void start_unfold();
    void accept_unfold_bundle(UnfoldBundle bundle);
    void advance_unfold();
    void cancel_unfold_activity();
    void finish_unfold_animation();
    void update_inspector(const SceneData& scene);
    void update_source_inspector();
    void rebuild_layer_tree();
    void apply_layer_state();
    void apply_scene_busy(bool busy);
    void apply_unfold_busy(bool busy);
    void refresh_interaction_state();
    void update_unfold_action();
    void set_view_actions_enabled(bool enabled);
    void select_mode_action(ViewMode mode);
    void select_content_action(MapContent content);
    void build_workbench();
    void apply_theme();

    std::shared_ptr<const source::Result> physical_world_;
    std::shared_ptr<const source::Result> political_world_;
    std::shared_ptr<const source::Result> world_;
    SceneController controller_;
    UnfoldController unfold_controller_;
    LayerStackState layer_stack_;
    MapCanvas* canvas_ = nullptr;
    QTreeWidget* layer_tree_ = nullptr;
    QLabel* source_value_ = nullptr;
    QLabel* content_value_ = nullptr;
    QLabel* mode_value_ = nullptr;
    QLabel* camera_value_ = nullptr;
    QLabel* geometry_value_ = nullptr;
    QLabel* state_value_ = nullptr;
    QAction* physical_action_ = nullptr;
    QAction* political_action_ = nullptr;
    QAction* globe_action_ = nullptr;
    QAction* sinusoidal_action_ = nullptr;
    QAction* mollweide_action_ = nullptr;
    QAction* unfold_action_ = nullptr;

    QTimer unfold_timer_;
    QElapsedTimer unfold_clock_;
    MapContent content_ = MapContent::physical;
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
