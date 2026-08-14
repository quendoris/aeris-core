// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "main_window.hpp"

#include "map_canvas.hpp"

#include <QAction>
#include <QActionGroup>
#include <QDockWidget>
#include <QFormLayout>
#include <QLabel>
#include <QStatusBar>
#include <QToolBar>
#include <QVBoxLayout>
#include <QWidget>

#include <utility>

namespace aeris::viewer {
namespace {

[[nodiscard]] QLabel* value_label(QWidget* parent) {
    auto* label = new QLabel(parent);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setWordWrap(true);
    return label;
}

}  // namespace

MainWindow::MainWindow(
    std::shared_ptr<const source::Result> world,
    const bool start_scene,
    QWidget* parent
)
    : QMainWindow(parent),
      world_(std::move(world)),
      controller_(world_, this) {
    build_workbench();
    apply_theme();

    controller_.set_scene_callback(
        [this](SceneData scene) { apply_scene(std::move(scene)); }
    );
    controller_.set_busy_callback(
        [this](const bool busy) { apply_busy(busy); }
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
            request.quality = final
                ? SceneQuality::verified
                : SceneQuality::preview;
            request.camera_longitude_deg = longitude_deg_;
            request.camera_latitude_deg = latitude_deg_;
            if (final) {
                controller_.request_verified(request);
            } else {
                controller_.request_preview(request);
            }
        }
    );

    resize(1280, 820);
    setWindowTitle(QStringLiteral("AERIS — Cartographic Workbench"));

    if (start_scene) {
        request_current_verified();
    }
}

void MainWindow::build_workbench() {
    canvas_ = new MapCanvas(this);
    setCentralWidget(canvas_);

    auto* views = new QToolBar(QStringLiteral("Views"), this);
    views->setOrientation(Qt::Vertical);
    views->setMovable(false);
    views->setFloatable(false);
    views->setToolButtonStyle(Qt::ToolButtonTextOnly);
    addToolBar(Qt::LeftToolBarArea, views);

    auto* group = new QActionGroup(this);
    group->setExclusive(true);

    auto add_view = [&](const QString& text, const ViewMode mode, const bool checked) {
        auto* action = views->addAction(text);
        action->setCheckable(true);
        action->setChecked(checked);
        group->addAction(action);
        connect(action, &QAction::triggered, this, [this, mode]() {
            set_mode(mode);
        });
    };

    add_view(QStringLiteral("Globe"), ViewMode::globe, true);
    add_view(QStringLiteral("Sin"), ViewMode::sinusoidal, false);
    add_view(QStringLiteral("Moll"), ViewMode::mollweide, false);
    views->addSeparator();
    auto* unfold = views->addAction(QStringLiteral("Unfold"));
    unfold->setEnabled(false);
    unfold->setToolTip(QStringLiteral(
        "Animation will be enabled after the two endpoint views share a stable interpolation contract."
    ));

    auto* source_dock = new QDockWidget(QStringLiteral("Source"), this);
    source_dock->setObjectName(QStringLiteral("sourceDock"));
    auto* source_widget = new QWidget(source_dock);
    auto* source_form = new QFormLayout(source_widget);
    source_value_ = value_label(source_widget);
    source_value_->setText(
        QStringLiteral("%1 / %2\n%3\n%4")
            .arg(QString::fromStdString(world_->provenance.provider))
            .arg(QString::fromStdString(world_->provenance.dataset))
            .arg(QString::fromStdString(world_->provenance.snapshot))
            .arg(QString::fromStdString(world_->provenance.content_sha256))
    );
    source_form->addRow(QStringLiteral("Verified"), source_value_);
    source_dock->setWidget(source_widget);
    addDockWidget(Qt::RightDockWidgetArea, source_dock);

    auto* view_dock = new QDockWidget(QStringLiteral("View"), this);
    view_dock->setObjectName(QStringLiteral("viewDock"));
    auto* view_widget = new QWidget(view_dock);
    auto* view_form = new QFormLayout(view_widget);
    mode_value_ = value_label(view_widget);
    camera_value_ = value_label(view_widget);
    geometry_value_ = value_label(view_widget);
    state_value_ = value_label(view_widget);
    mode_value_->setText(QStringLiteral("Globe"));
    camera_value_->setText(QStringLiteral("15.00°, 20.00°"));
    geometry_value_->setText(QStringLiteral("—"));
    state_value_->setText(QStringLiteral("Idle"));
    view_form->addRow(QStringLiteral("Mode"), mode_value_);
    view_form->addRow(QStringLiteral("Camera"), camera_value_);
    view_form->addRow(QStringLiteral("Geometry"), geometry_value_);
    view_form->addRow(QStringLiteral("State"), state_value_);
    view_dock->setWidget(view_widget);
    addDockWidget(Qt::RightDockWidgetArea, view_dock);

    statusBar()->showMessage(
        QStringLiteral("Pinned Natural Earth loaded and verified")
    );
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
        QLabel { background: transparent; }
        QStatusBar { background: #202329; color: #aeb4bd; }
    )"));
}

void MainWindow::set_mode(const ViewMode mode) {
    if (mode_ == mode) {
        return;
    }
    mode_ = mode;
    mode_value_->setText(QString::fromLatin1(view_mode_name(mode_)));
    camera_value_->setEnabled(mode_ == ViewMode::globe);
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

void MainWindow::apply_scene(SceneData scene) {
    if (!scene.ok) {
        statusBar()->showMessage(
            QStringLiteral("Scene failed: %1")
                .arg(QString::fromStdString(scene.diagnostic))
        );
    } else {
        statusBar()->showMessage(QString::fromStdString(scene.diagnostic));
    }
    update_inspector(scene);
    canvas_->set_scene(std::move(scene));
}

void MainWindow::apply_busy(const bool busy) {
    canvas_->set_busy(busy);
    state_value_->setText(
        busy ? QStringLiteral("Verifying…") : QStringLiteral("Ready")
    );
}

void MainWindow::update_inspector(const SceneData& scene) {
    mode_value_->setText(QString::fromLatin1(view_mode_name(scene.mode)));
    geometry_value_->setText(
        QStringLiteral("%1 fill rings\n%2 outline parts\n%3 vertices\nmax refinement %4")
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
