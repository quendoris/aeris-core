// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "map_canvas.hpp"

#include "render_surface.hpp"

#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <utility>

namespace aeris::viewer {
namespace {

[[nodiscard]] double wrap_longitude(double value) noexcept {
    value = std::fmod(value + 180.0, 360.0);
    if (value < 0.0) value += 360.0;
    return value - 180.0;
}

}  // namespace

MapCanvas::MapCanvas(QWidget* parent)
    : QWidget(parent) {
    setMinimumSize(640, 480);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
}

void MapCanvas::set_scene(SceneData scene) {
    unfold_.reset();
    unfold_progress_ = 0.0;
    scene_ = std::move(scene);
    longitude_deg_ = scene_.camera_longitude_deg;
    latitude_deg_ = scene_.camera_latitude_deg;
    update();
}

void MapCanvas::set_busy(const bool busy) {
    busy_ = busy;
    update();
}

void MapCanvas::set_layer_render_state(const LayerRenderState state) {
    layer_render_state_ = state;
    update();
}

void MapCanvas::set_camera_callback(CameraCallback callback) {
    camera_callback_ = std::move(callback);
}

void MapCanvas::set_camera(
    const double longitude_deg,
    const double latitude_deg
) {
    longitude_deg_ = wrap_longitude(longitude_deg);
    latitude_deg_ = std::clamp(latitude_deg, -89.5, 89.5);
    update();
}

void MapCanvas::begin_unfold(UnfoldBundle bundle) {
    longitude_deg_ = bundle.globe_endpoint.camera_longitude_deg;
    latitude_deg_ = bundle.globe_endpoint.camera_latitude_deg;
    unfold_progress_ = 0.0;
    unfold_ = std::move(bundle);
    update();
}

void MapCanvas::set_unfold_progress(const double progress) {
    unfold_progress_ = std::clamp(progress, 0.0, 1.0);
    update();
}

const SceneData& MapCanvas::finish_unfold() {
    if (unfold_) {
        scene_ = std::move(unfold_->flat_endpoint);
        longitude_deg_ = scene_.camera_longitude_deg;
        latitude_deg_ = scene_.camera_latitude_deg;
        unfold_.reset();
        unfold_progress_ = 0.0;
        update();
    }
    return scene_;
}

void MapCanvas::cancel_unfold() {
    if (unfold_) {
        scene_ = std::move(unfold_->globe_endpoint);
        longitude_deg_ = scene_.camera_longitude_deg;
        latitude_deg_ = scene_.camera_latitude_deg;
        unfold_.reset();
        unfold_progress_ = 0.0;
        update();
    }
}

void MapCanvas::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), QColor(24, 26, 30));

    if (unfold_) {
        const double t = unfold_eased_progress(unfold_progress_);
        const CanvasBounds bounds = interpolate_bounds(
            scene_bounds(unfold_->globe_endpoint),
            scene_bounds(unfold_->flat_endpoint),
            t
        );
        apply_world_transform(painter, bounds, width(), height(), zoom_);
        LayerRenderState transition_layers = layer_render_state_;
        transition_layers.labels_visible = false;
        draw_scene_geometry(painter, unfold_->globe_endpoint, 1.0 - t, transition_layers);
        draw_scene_geometry(painter, unfold_->flat_endpoint, t, transition_layers);
        draw_unfold_guides(painter, *unfold_, unfold_progress_);

        painter.resetTransform();
        painter.setOpacity(1.0);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(18, 20, 23, 225));
        painter.drawRoundedRect(QRect(18, 16, 440, 82), 8, 8);
        painter.setBrush(QColor(170, 126, 201));
        painter.drawRoundedRect(QRect(30, 28, 96, 24), 6, 6);
        painter.setPen(QColor(245, 246, 242));
        painter.drawText(QRect(30, 28, 96, 24), Qt::AlignCenter, QStringLiteral("UNFOLD"));
        painter.drawText(
            QPoint(30, 72),
            unfold_->globe_endpoint.political
                ? QStringLiteral("Political · Globe → %1")
                    .arg(QString::fromLatin1(view_mode_name(unfold_->target_mode)))
                : QStringLiteral("Globe → %1")
                    .arg(QString::fromLatin1(view_mode_name(unfold_->target_mode)))
        );
        painter.setPen(QColor(179, 183, 190));
        painter.drawText(
            QPoint(145, 46),
            QStringLiteral("%1%").arg(static_cast<int>(std::lround(unfold_progress_ * 100.0)))
        );
        painter.drawText(
            QRect(18, height() - 48, width() - 36, 30),
            Qt::AlignLeft | Qt::AlignVCenter,
            QStringLiteral("Explanatory transition — verified endpoints remain authoritative")
        );
        return;
    }

    apply_world_transform(painter, scene_bounds(scene_), width(), height(), zoom_);
    draw_scene_geometry(painter, scene_, 1.0, layer_render_state_);

    painter.resetTransform();
    painter.setRenderHint(QPainter::Antialiasing, true);

    const bool verified = scene_.quality == SceneQuality::verified && scene_.ok;
    const QString quality = busy_
        ? QStringLiteral("VERIFYING")
        : verified ? QStringLiteral("VERIFIED") : QStringLiteral("PREVIEW");
    const QColor badge = busy_
        ? QColor(186, 149, 74)
        : verified ? QColor(88, 174, 125) : QColor(101, 143, 194);

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(18, 20, 23, 225));
    painter.drawRoundedRect(QRect(18, 16, 380, 82), 8, 8);
    painter.setBrush(badge);
    painter.drawRoundedRect(QRect(30, 28, 96, 24), 6, 6);
    painter.setPen(QColor(245, 246, 242));
    painter.drawText(QRect(30, 28, 96, 24), Qt::AlignCenter, quality);
    painter.drawText(QPoint(30, 72), scene_caption(scene_));

    if (scene_.mode == ViewMode::globe) {
        painter.setPen(QColor(165, 170, 178));
        painter.drawText(
            QPoint(145, 46),
            QStringLiteral("%1°, %2°")
                .arg(longitude_deg_, 0, 'f', 1)
                .arg(latitude_deg_, 0, 'f', 1)
        );
    }

    if (!scene_.ok) {
        painter.setPen(QColor(235, 113, 113));
        painter.drawText(
            QRect(18, height() - 64, width() - 36, 46),
            Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap,
            QString::fromStdString(scene_.diagnostic)
        );
    } else if (scene_.quality == SceneQuality::preview) {
        painter.setPen(QColor(165, 170, 178));
        painter.drawText(
            QRect(18, height() - 48, width() - 36, 30),
            Qt::AlignLeft | Qt::AlignVCenter,
            QString::fromStdString(scene_.diagnostic)
        );
    }
}

void MapCanvas::mousePressEvent(QMouseEvent* event) {
    if (unfold_ || scene_.mode != ViewMode::globe || event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    dragging_ = true;
    last_mouse_ = event->pos();
    setCursor(Qt::ClosedHandCursor);
    event->accept();
}

void MapCanvas::mouseMoveEvent(QMouseEvent* event) {
    if (unfold_ || !dragging_ || scene_.mode != ViewMode::globe) {
        QWidget::mouseMoveEvent(event);
        return;
    }

    const QPoint delta = event->pos() - last_mouse_;
    last_mouse_ = event->pos();
    longitude_deg_ = wrap_longitude(longitude_deg_ - static_cast<double>(delta.x()) * 0.35);
    latitude_deg_ = std::clamp(
        latitude_deg_ + static_cast<double>(delta.y()) * 0.35,
        -89.5, 89.5
    );
    emit_camera(false);
    event->accept();
}

void MapCanvas::mouseReleaseEvent(QMouseEvent* event) {
    if (unfold_ || !dragging_ || event->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    dragging_ = false;
    unsetCursor();
    emit_camera(true);
    event->accept();
}

void MapCanvas::wheelEvent(QWheelEvent* event) {
    const double factor = event->angleDelta().y() > 0 ? 1.12 : 1.0 / 1.12;
    zoom_ = std::clamp(zoom_ * factor, 0.55, 8.0);
    update();
    event->accept();
}

void MapCanvas::emit_camera(const bool final) {
    if (camera_callback_) camera_callback_(longitude_deg_, latitude_deg_, final);
}

}  // namespace aeris::viewer
