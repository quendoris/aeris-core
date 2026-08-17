// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "main_window.hpp"

#include "map_canvas.hpp"

#include <QAbstractItemView>
#include <QAction>
#include <QActionGroup>
#include <QDockWidget>
#include <QFormLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QToolBar>
#include <QTreeWidget>
#include <QWidget>

#include <algorithm>
#include <string_view>
#include <utility>

namespace aeris::viewer {
namespace {

constexpr qint64 kUnfoldDurationMs = 1400;

[[nodiscard]] QLabel* value_label(QWidget* parent) {
    auto* label = new QLabel(parent);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setWordWrap(true);
    return label;
}

[[nodiscard]] QString compact_content_identity(const std::string& hash) {
    const QString value = QString::fromStdString(hash);
    constexpr int visible_end = 12;
    if (value.size() <= 2 * visible_end + 1) return value;
    return QStringLiteral("%1…%2")
        .arg(value.left(visible_end))
        .arg(value.right(visible_end));
}

[[nodiscard]] QString latin1_view(const std::string_view value) {
    return QString::fromLatin1(value.data(), static_cast<qsizetype>(value.size()));
}

}  // namespace

MainWindow::MainWindow(
    std::shared_ptr<const source::Result> physical_world,
    std::shared_ptr<const source::Result> political_world,
    const MapContent initial_content,
    const bool start_scene,
    QWidget* parent
)
    : QMainWindow(parent),
      physical_world_(std::move(physical_world)),
      political_world_(std::move(political_world)),
      world_(initial_content == MapContent::political ? political_world_ : physical_world_),
      controller_(world_, this),
      unfold_controller_(world_, this),
      layer_stack_(initial_content),
      content_(initial_content) {
    build_workbench();
    apply_theme();

    controller_.set_scene_callback(
        [this](SceneData scene) { present_scene(std::move(scene)); }
    );
    controller_.set_busy_callback(
        [this](const bool busy) { apply_scene_busy(busy); }
    );
    unfold_controller_.set_bundle_callback(
        [this](UnfoldBundle bundle) { accept_unfold_bundle(std::move(bundle)); }
    );
    unfold_controller_.set_busy_callback(
        [this](const bool busy) { apply_unfold_busy(busy); }
    );
    canvas_->set_camera_callback(
        [this](const double longitude, const double latitude, const bool final) {
            longitude_deg_ = longitude;
            latitude_deg_ = latitude;
            camera_value_->setText(
                QStringLiteral("%1°, %2°")
                    .arg(longitude_deg_, 0, 'f', 2)
                    .arg(latitude_deg_, 0, 'f', 2)
            );

            SceneRequest request{};
            request.mode = ViewMode::globe;
            request.quality = final ? SceneQuality::verified : SceneQuality::preview;
            request.camera_longitude_deg = longitude_deg_;
            request.camera_latitude_deg = latitude_deg_;
            scene_verified_ = false;
            update_unfold_action();
            if (final) controller_.request_verified(request);
            else controller_.request_preview(request);
        }
    );

    unfold_timer_.setInterval(16);
    connect(&unfold_timer_, &QTimer::timeout, this, [this]() { advance_unfold(); });

    resize(1280, 820);
    setWindowTitle(QStringLiteral("AERIS — Cartographic Workbench"));
    if (start_scene) request_current_verified();
}

void MainWindow::build_workbench() {
    canvas_ = new MapCanvas(this);
    setCentralWidget(canvas_);

    auto* views = new QToolBar(QStringLiteral("Workbench"), this);
    views->setOrientation(Qt::Vertical);
    views->setMovable(false);
    views->setFloatable(false);
    views->setToolButtonStyle(Qt::ToolButtonTextOnly);
    addToolBar(Qt::LeftToolBarArea, views);

    auto* content_group = new QActionGroup(this);
    content_group->setExclusive(true);
    auto add_content = [&](const QString& text, const MapContent content) {
        auto* action = views->addAction(text);
        action->setCheckable(true);
        action->setChecked(content_ == content);
        content_group->addAction(action);
        connect(action, &QAction::triggered, this, [this, content]() { set_content(content); });
        return action;
    };
    physical_action_ = add_content(QStringLiteral("Physical"), MapContent::physical);
    political_action_ = add_content(QStringLiteral("Political"), MapContent::political);

    views->addSeparator();
    auto* view_group = new QActionGroup(this);
    view_group->setExclusive(true);
    auto add_view = [&](const QString& text, const ViewMode mode, const bool checked) {
        auto* action = views->addAction(text);
        action->setCheckable(true);
        action->setChecked(checked);
        view_group->addAction(action);
        connect(action, &QAction::triggered, this, [this, mode]() { set_mode(mode); });
        return action;
    };
    globe_action_ = add_view(QStringLiteral("Globe"), ViewMode::globe, true);
    sinusoidal_action_ = add_view(QStringLiteral("Sin"), ViewMode::sinusoidal, false);
    mollweide_action_ = add_view(QStringLiteral("Moll"), ViewMode::mollweide, false);
    views->addSeparator();
    unfold_action_ = views->addAction(QStringLiteral("Unfold"));
    unfold_action_->setEnabled(false);
    connect(unfold_action_, &QAction::triggered, this, [this]() { start_unfold(); });

    auto* layers_dock = new QDockWidget(QStringLiteral("Layers"), this);
    layers_dock->setObjectName(QStringLiteral("layersDock"));
    layer_tree_ = new QTreeWidget(layers_dock);
    layer_tree_->setHeaderHidden(true);
    layer_tree_->setRootIsDecorated(false);
    layer_tree_->setItemsExpandable(false);
    layer_tree_->setSelectionMode(QAbstractItemView::SingleSelection);
    layer_tree_->setMinimumWidth(235);
    layer_tree_->setMinimumHeight(170);
    layers_dock->setWidget(layer_tree_);
    addDockWidget(Qt::RightDockWidgetArea, layers_dock);

    connect(
        layer_tree_,
        &QTreeWidget::itemChanged,
        this,
        [this](QTreeWidgetItem* item, const int column) {
            if (item == nullptr || column != 0) return;
            const std::string layer_id = item->data(0, Qt::UserRole).toString().toStdString();
            const bool visible = item->checkState(0) == Qt::Checked;
            if (!layer_stack_.set_visible(layer_id, visible)) return;
            apply_layer_state();
            statusBar()->showMessage(
                QStringLiteral("%1 layer %2")
                    .arg(item->text(0))
                    .arg(visible ? QStringLiteral("enabled") : QStringLiteral("hidden")),
                1800
            );
        }
    );

    auto* inspector_dock = new QDockWidget(QStringLiteral("Inspector"), this);
    inspector_dock->setObjectName(QStringLiteral("inspectorDock"));
    auto* inspector_widget = new QWidget(inspector_dock);
    inspector_widget->setMinimumWidth(235);
    auto* inspector_form = new QFormLayout(inspector_widget);
    source_value_ = value_label(inspector_widget);
    content_value_ = value_label(inspector_widget);
    mode_value_ = value_label(inspector_widget);
    camera_value_ = value_label(inspector_widget);
    geometry_value_ = value_label(inspector_widget);
    state_value_ = value_label(inspector_widget);
    content_value_->setText(QString::fromLatin1(map_content_name(content_)));
    mode_value_->setText(QStringLiteral("Globe"));
    camera_value_->setText(QStringLiteral("15.00°, 20.00°"));
    geometry_value_->setText(QStringLiteral("—"));
    state_value_->setText(QStringLiteral("Idle"));
    inspector_form->addRow(QStringLiteral("Source"), source_value_);
    inspector_form->addRow(QStringLiteral("Content"), content_value_);
    inspector_form->addRow(QStringLiteral("Mode"), mode_value_);
    inspector_form->addRow(QStringLiteral("Camera"), camera_value_);
    inspector_form->addRow(QStringLiteral("Geometry"), geometry_value_);
    inspector_form->addRow(QStringLiteral("State"), state_value_);
    inspector_dock->setWidget(inspector_widget);
    addDockWidget(Qt::RightDockWidgetArea, inspector_dock);
    splitDockWidget(layers_dock, inspector_dock, Qt::Vertical);
    resizeDocks({layers_dock, inspector_dock}, {220, 470}, Qt::Vertical);

    rebuild_layer_tree();
    apply_layer_state();
    update_source_inspector();
    statusBar()->showMessage(QStringLiteral("Pinned Natural Earth sources loaded and verified"));
    update_unfold_action();
}

void MainWindow::apply_theme() {
    setStyleSheet(QStringLiteral(R"(
        QMainWindow, QWidget { background: #181a1e; color: #e8e9e5; }
        QToolBar { background: #202329; border: none; spacing: 6px; padding: 8px 5px; }
        QToolButton { background: #2a2e35; border: 1px solid #343943; border-radius: 5px; padding: 8px 7px; }
        QToolButton:checked { background: #3a4655; border-color: #61748b; }
        QToolButton:disabled { color: #777d86; background: #202329; }
        QDockWidget { color: #d9dcd7; }
        QDockWidget::title { background: #202329; padding: 7px; }
        QTreeWidget { background: #202329; border: none; outline: none; padding: 5px; }
        QTreeWidget::item { padding: 7px 4px; border-radius: 4px; }
        QTreeWidget::item:selected { background: #303640; color: #f0f1ed; }
        QLabel { background: transparent; }
        QStatusBar { background: #202329; color: #aeb4bd; }
    )"));
}

void MainWindow::set_content(const MapContent content) {
    if (unfold_animating_) return;
    if (content_ == content && scene_verified_) return;

    cancel_unfold_activity();
    content_ = content;
    layer_stack_.set_content(content_);
    world_ = content_ == MapContent::political ? political_world_ : physical_world_;
    controller_.set_world(world_);
    unfold_controller_.set_world(world_);
    select_content_action(content_);
    content_value_->setText(QString::fromLatin1(map_content_name(content_)));
    rebuild_layer_tree();
    apply_layer_state();
    update_source_inspector();
    scene_verified_ = false;
    statusBar()->showMessage(
        QStringLiteral("Switching to verified %1 source…")
            .arg(QString::fromLatin1(map_content_name(content_)))
    );
    request_current_verified();
}

void MainWindow::set_mode(const ViewMode mode) {
    if (unfold_animating_) return;
    if (unfold_preparing_) {
        unfold_controller_.cancel();
        unfold_preparing_ = false;
    }
    if (mode_ == mode && scene_verified_) {
        update_unfold_action();
        return;
    }

    mode_ = mode;
    if (mode != ViewMode::globe) last_flat_mode_ = mode;
    mode_value_->setText(QString::fromLatin1(view_mode_name(mode_)));
    camera_value_->setEnabled(mode_ == ViewMode::globe);
    scene_verified_ = false;
    update_unfold_action();
    request_current_verified();
}

void MainWindow::request_current_verified() {
    SceneRequest request{};
    request.mode = mode_;
    request.quality = SceneQuality::verified;
    request.camera_longitude_deg = longitude_deg_;
    request.camera_latitude_deg = latitude_deg_;
    controller_.request_verified(request);
}

void MainWindow::start_unfold() {
    if (mode_ != ViewMode::globe || !scene_verified_ ||
        scene_busy_ || unfold_preparing_ || unfold_animating_) return;

    controller_.cancel();
    unfold_target_ = last_flat_mode_;
    unfold_controller_.request(longitude_deg_, latitude_deg_, unfold_target_);
    statusBar()->showMessage(
        QStringLiteral("Preparing verified %1 Globe → %2 endpoints…")
            .arg(QString::fromLatin1(map_content_name(content_)))
            .arg(QString::fromLatin1(view_mode_name(unfold_target_)))
    );
}

void MainWindow::accept_unfold_bundle(UnfoldBundle bundle) {
    if (!bundle.ok || bundle.canceled) {
        statusBar()->showMessage(
            QStringLiteral("Unfold preparation failed: %1")
                .arg(QString::fromStdString(bundle.diagnostic))
        );
        update_unfold_action();
        return;
    }

    unfold_target_ = bundle.target_mode;
    canvas_->begin_unfold(std::move(bundle));
    unfold_animating_ = true;
    scene_verified_ = false;
    set_view_actions_enabled(false);
    state_value_->setText(QStringLiteral("Transition (non-normative)"));
    mode_value_->setText(
        QStringLiteral("Unfold → %1").arg(QString::fromLatin1(view_mode_name(unfold_target_)))
    );
    statusBar()->showMessage(QStringLiteral(
        "Animating explanatory transition; verified endpoints remain authoritative"
    ));
    unfold_clock_.restart();
    unfold_timer_.start();
}

void MainWindow::advance_unfold() {
    if (!unfold_animating_ || !canvas_->is_unfolding()) {
        unfold_timer_.stop();
        return;
    }
    const double progress = std::clamp(
        static_cast<double>(unfold_clock_.elapsed()) / static_cast<double>(kUnfoldDurationMs),
        0.0, 1.0
    );
    canvas_->set_unfold_progress(progress);
    state_value_->setText(
        QStringLiteral("Transition %1% (non-normative)")
            .arg(static_cast<int>(progress * 100.0))
    );
    if (progress >= 1.0) finish_unfold_animation();
}

void MainWindow::finish_unfold_animation() {
    unfold_timer_.stop();
    const SceneData& final_scene = canvas_->finish_unfold();
    unfold_animating_ = false;
    mode_ = unfold_target_;
    last_flat_mode_ = mode_;
    scene_verified_ = final_scene.ok && final_scene.quality == SceneQuality::verified;
    select_mode_action(mode_);
    set_view_actions_enabled(true);
    camera_value_->setEnabled(false);
    update_inspector(final_scene);
    statusBar()->showMessage(QString::fromStdString(final_scene.diagnostic));
    update_unfold_action();
}

void MainWindow::cancel_unfold_activity() {
    unfold_controller_.cancel();
    unfold_preparing_ = false;
    if (unfold_animating_) {
        unfold_timer_.stop();
        canvas_->cancel_unfold();
        unfold_animating_ = false;
        scene_verified_ = canvas_->scene().ok &&
            canvas_->scene().quality == SceneQuality::verified;
        set_view_actions_enabled(true);
    }
    update_unfold_action();
}

void MainWindow::present_scene(SceneData scene) {
    cancel_unfold_activity();
    mode_ = scene.mode;
    if (mode_ != ViewMode::globe) last_flat_mode_ = mode_;
    scene_verified_ = scene.ok && scene.quality == SceneQuality::verified;
    select_mode_action(mode_);
    select_content_action(content_);
    camera_value_->setEnabled(mode_ == ViewMode::globe);
    if (!scene.ok) {
        statusBar()->showMessage(
            QStringLiteral("Scene failed: %1").arg(QString::fromStdString(scene.diagnostic))
        );
    } else {
        statusBar()->showMessage(QString::fromStdString(scene.diagnostic));
    }
    update_inspector(scene);
    canvas_->set_scene(std::move(scene));
    apply_layer_state();
    update_unfold_action();
}

void MainWindow::present_unfold_frame(UnfoldBundle bundle, const double progress) {
    cancel_unfold_activity();
    unfold_target_ = bundle.target_mode;
    canvas_->begin_unfold(std::move(bundle));
    canvas_->set_unfold_progress(progress);
    unfold_animating_ = true;
    scene_verified_ = false;
    set_view_actions_enabled(false);
    mode_value_->setText(
        QStringLiteral("Unfold → %1").arg(QString::fromLatin1(view_mode_name(unfold_target_)))
    );
    state_value_->setText(
        QStringLiteral("Transition %1% (non-normative)")
            .arg(static_cast<int>(std::clamp(progress, 0.0, 1.0) * 100.0))
    );
}

void MainWindow::rebuild_layer_tree() {
    if (layer_tree_ == nullptr) return;
    const QSignalBlocker blocker(layer_tree_);
    layer_tree_->clear();
    for (const LayerDescriptor& layer : layer_stack_.active_layers()) {
        auto* item = new QTreeWidgetItem(layer_tree_);
        item->setText(0, latin1_view(layer.name));
        item->setData(0, Qt::UserRole, latin1_view(layer.layer_id));
        item->setCheckState(0, layer.visible ? Qt::Checked : Qt::Unchecked);
        item->setToolTip(
            0,
            QStringLiteral("role: %1").arg(latin1_view(layer.role_id))
        );
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsSelectable);
    }
}

void MainWindow::apply_layer_state() {
    if (canvas_ != nullptr) canvas_->set_layer_render_state(layer_stack_.render_state());
}

void MainWindow::apply_scene_busy(const bool busy) {
    scene_busy_ = busy;
    refresh_interaction_state();
}

void MainWindow::apply_unfold_busy(const bool busy) {
    unfold_preparing_ = busy;
    refresh_interaction_state();
}

void MainWindow::refresh_interaction_state() {
    canvas_->set_busy(scene_busy_ || unfold_preparing_);
    if (unfold_animating_) return;
    if (unfold_preparing_) state_value_->setText(QStringLiteral("Preparing unfold…"));
    else if (scene_busy_) state_value_->setText(QStringLiteral("Verifying…"));
    else if (!scene_verified_) state_value_->setText(QStringLiteral("Ready"));
    update_unfold_action();
}

void MainWindow::update_unfold_action() {
    if (unfold_action_ == nullptr) return;
    unfold_action_->setEnabled(
        mode_ == ViewMode::globe && scene_verified_ &&
        !scene_busy_ && !unfold_preparing_ && !unfold_animating_
    );
    unfold_action_->setToolTip(
        QStringLiteral("Animate verified %1 Globe → %2. Intermediate frames are explanatory only.")
            .arg(QString::fromLatin1(map_content_name(content_)))
            .arg(QString::fromLatin1(view_mode_name(last_flat_mode_)))
    );
}

void MainWindow::set_view_actions_enabled(const bool enabled) {
    physical_action_->setEnabled(enabled);
    political_action_->setEnabled(enabled);
    globe_action_->setEnabled(enabled);
    sinusoidal_action_->setEnabled(enabled);
    mollweide_action_->setEnabled(enabled);
    if (layer_tree_ != nullptr) layer_tree_->setEnabled(enabled);
    if (!enabled) unfold_action_->setEnabled(false);
    else update_unfold_action();
}

void MainWindow::select_mode_action(const ViewMode mode) {
    globe_action_->setChecked(mode == ViewMode::globe);
    sinusoidal_action_->setChecked(mode == ViewMode::sinusoidal);
    mollweide_action_->setChecked(mode == ViewMode::mollweide);
}

void MainWindow::select_content_action(const MapContent content) {
    physical_action_->setChecked(content == MapContent::physical);
    political_action_->setChecked(content == MapContent::political);
}

void MainWindow::update_source_inspector() {
    const QString full_hash = QString::fromStdString(world_->provenance.content_sha256);
    source_value_->setText(
        QStringLiteral("%1 / %2\n%3\nSHA-256 %4%5")
            .arg(QString::fromStdString(world_->provenance.provider))
            .arg(QString::fromStdString(world_->provenance.dataset))
            .arg(QString::fromStdString(world_->provenance.snapshot))
            .arg(compact_content_identity(world_->provenance.content_sha256))
            .arg(world_->provenance.worldview.empty()
                ? QString()
                : QStringLiteral("\nworldview %1")
                    .arg(QString::fromStdString(world_->provenance.worldview)))
    );
    source_value_->setToolTip(
        QStringLiteral("Full verified content SHA-256:\n%1").arg(full_hash)
    );
}

void MainWindow::update_inspector(const SceneData& scene) {
    content_value_->setText(QString::fromLatin1(map_content_name(content_)));
    mode_value_->setText(QString::fromLatin1(view_mode_name(scene.mode)));
    geometry_value_->setText(
        QStringLiteral("%1 features\n%2 fill rings\n%3 outline parts\n%4 vertices\nmax refinement %5")
            .arg(scene.features.size())
            .arg(scene.fill_rings)
            .arg(scene.outline_parts)
            .arg(scene.vertices)
            .arg(scene.max_refinement_rounds)
    );
    state_value_->setText(
        scene.ok
            ? scene.quality == SceneQuality::verified
                ? QStringLiteral("Verified")
                : QStringLiteral("Preview")
            : QStringLiteral("Error")
    );
}

}  // namespace aeris::viewer
