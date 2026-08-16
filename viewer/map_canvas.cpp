// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "map_canvas.hpp"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <utility>

namespace aeris::viewer {
namespace {

constexpr int kMarginPx = 52;
constexpr double kPi = 3.141592653589793238462643383279502884;

struct Bounds final {
    double min_x = -1.0;
    double min_y = -1.0;
    double max_x = 1.0;
    double max_y = 1.0;
};

[[nodiscard]] double wrap_longitude(double value) noexcept {
    value = std::fmod(value + 180.0, 360.0);
    if (value < 0.0) {
        value += 360.0;
    }
    return value - 180.0;
}

[[nodiscard]] Bounds scene_bounds(const SceneData& scene) noexcept {
    return {scene.min_x, scene.min_y, scene.max_x, scene.max_y};
}

[[nodiscard]] Bounds interpolate_bounds(
    const Bounds& from,
    const Bounds& to,
    const double t
) noexcept {
    return {
        from.min_x + (to.min_x - from.min_x) * t,
        from.min_y + (to.min_y - from.min_y) * t,
        from.max_x + (to.max_x - from.max_x) * t,
        from.max_y + (to.max_y - from.max_y) * t,
    };
}

void apply_world_transform(
    QPainter& painter,
    const Bounds& bounds,
    const int width,
    const int height,
    const double zoom
) {
    const double span_x = std::max(1.0, bounds.max_x - bounds.min_x);
    const double span_y = std::max(1.0, bounds.max_y - bounds.min_y);
    const double available_w = std::max(1, width - 2 * kMarginPx);
    const double available_h = std::max(1, height - 2 * kMarginPx);
    const double scale = zoom * std::min(available_w / span_x, available_h / span_y);
    const double center_x = 0.5 * (bounds.min_x + bounds.max_x);
    const double center_y = 0.5 * (bounds.min_y + bounds.max_y);

    QTransform transform;
    transform.translate(0.5 * static_cast<double>(width), 0.5 * static_cast<double>(height));
    transform.scale(scale, -scale);
    transform.translate(-center_x, -center_y);
    painter.setWorldTransform(transform);
}

void draw_scene_geometry(
    QPainter& painter,
    const SceneData& scene,
    const double opacity
) {
    if (opacity <= 0.0) {
        return;
    }

    painter.save();
    painter.setOpacity(std::clamp(opacity, 0.0, 1.0));

    if (scene.mode == ViewMode::globe && scene.globe_radius_m > 0.0) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(42, 48, 56));
        painter.drawEllipse(
            QPointF(0.0, 0.0),
            scene.globe_radius_m,
            scene.globe_radius_m
        );
    }

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(197, 198, 188));
    for (const auto& feature : scene.features) {
        if (feature.fill_rings.empty()) {
            continue;
        }
        QPainterPath path;
        path.setFillRule(Qt::OddEvenFill);
        for (const auto& ring : feature.fill_rings) {
            if (ring.size() < 3U) {
                continue;
            }
            path.moveTo(ring.front().x, ring.front().y);
            for (std::size_t index = 1U; index < ring.size(); ++index) {
                path.lineTo(ring[index].x, ring[index].y);
            }
            path.closeSubpath();
        }
        painter.drawPath(path);
    }

    QPen coastline(QColor(230, 231, 226));
    coastline.setWidthF(1.0);
    coastline.setCosmetic(true);
    coastline.setJoinStyle(Qt::RoundJoin);
    coastline.setCapStyle(Qt::RoundCap);
    painter.setPen(coastline);
    painter.setBrush(Qt::NoBrush);
    for (const auto& feature : scene.features) {
        for (const auto& line : feature.outlines) {
            if (line.size() < 2U) {
                continue;
            }
            QPainterPath path;
            path.moveTo(line.front().x, line.front().y);
            for (std::size_t index = 1U; index < line.size(); ++index) {
                path.lineTo(line[index].x, line[index].y);
            }
            if (scene.mode != ViewMode::globe && line.size() >= 3U) {
                path.closeSubpath();
            }
            painter.drawPath(path);
        }
    }

    if (scene.mode == ViewMode::globe && scene.globe_radius_m > 0.0) {
        QPen limb(QColor(122, 132, 146));
        limb.setWidthF(1.25);
        limb.setCosmetic(true);
        painter.setPen(limb);
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(
            QPointF(0.0, 0.0),
            scene.globe_radius_m,
            scene.globe_radius_m
        );
    }

    painter.restore();
}

void draw_unfold_guides(
    QPainter& painter,
    const UnfoldBundle& bundle,
    const double progress
) {
    const double t = unfold_eased_progress(progress);
    const double transition_alpha = std::sin(kPi * t);
    if (transition_alpha <= 0.0) {
        return;
    }

    for (const auto& line : bundle.guides) {
        if (line.vertices.size() < 2U) {
            continue;
        }

        QPen pen(
            line.kind == UnfoldGuideKind::seam
                ? QColor(226, 171, 92)
                : QColor(113, 151, 187)
        );
        pen.setWidthF(line.kind == UnfoldGuideKind::seam ? 1.7 : 1.0);
        pen.setCosmetic(true);
        pen.setCapStyle(Qt::RoundCap);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);

        for (std::size_t index = 1U; index < line.vertices.size(); ++index) {
            const auto& a = line.vertices[index - 1U];
            const auto& b = line.vertices[index];
            const double visibility = std::min(
                unfold_guide_visibility(a, progress),
                unfold_guide_visibility(b, progress)
            );
            const double alpha = transition_alpha * visibility *
                (line.kind == UnfoldGuideKind::seam ? 0.95 : 0.62);
            if (alpha <= 0.0) {
                continue;
            }

            const auto pa = interpolate_unfold_vertex(a, progress);
            const auto pb = interpolate_unfold_vertex(b, progress);
            painter.save();
            painter.setOpacity(std::clamp(alpha, 0.0, 1.0));
            painter.drawLine(QPointF(pa.x, pa.y), QPointF(pb.x, pb.y));
            painter.restore();
        }
    }
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
        const Bounds bounds = interpolate_bounds(
            scene_bounds(unfold_->globe_endpoint),
            scene_bounds(unfold_->flat_endpoint),
            t
        );
        apply_world_transform(painter, bounds, width(), height(), zoom_);
        draw_scene_geometry(painter, unfold_->globe_endpoint, 1.0 - t);
        draw_scene_geometry(painter, unfold_->flat_endpoint, t);
        draw_unfold_guides(painter, *unfold_, unfold_progress_);

        painter.resetTransform();
        painter.setOpacity(1.0);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(18, 20, 23, 225));
        painter.drawRoundedRect(QRect(18, 16, 390, 82), 8, 8);
        painter.setBrush(QColor(170, 126, 201));
        painter.drawRoundedRect(QRect(30, 28, 96, 24), 6, 6);
        painter.setPen(QColor(245, 246, 242));
        painter.drawText(QRect(30, 28, 96, 24), Qt::AlignCenter, QStringLiteral("UNFOLD"));
        painter.drawText(
            QPoint(30, 72),
            QStringLiteral("Globe → %1")
                .arg(QString::fromLatin1(view_mode_name(unfold_->target_mode)))
        );
        painter.setPen(QColor(179, 183, 190));
        painter.drawText(
            QPoint(145, 46),
            QStringLiteral("%1%").arg(
                static_cast<int>(std::lround(unfold_progress_ * 100.0))
            )
        );
        painter.drawText(
            QRect(18, height() - 48, width() - 36, 30),
            Qt::AlignLeft | Qt::AlignVCenter,
            QStringLiteral("Explanatory transition — verified endpoints remain authoritative")
        );
        return;
    }

    apply_world_transform(painter, scene_bounds(scene_), width(), height(), zoom_);
    draw_scene_geometry(painter, scene_, 1.0);

    painter.resetTransform();
    painter.setRenderHint(QPainter::Antialiasing, true);

    const bool verified = scene_.quality == SceneQuality::verified && scene_.ok;
    const QString quality = busy_
        ? QStringLiteral("VERIFYING")
        : verified
            ? QStringLiteral("VERIFIED")
            : QStringLiteral("PREVIEW");

    const QColor badge = busy_
        ? QColor(186, 149, 74)
        : verified
            ? QColor(88, 174, 125)
            : QColor(101, 143, 194);

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(18, 20, 23, 225));
    painter.drawRoundedRect(QRect(18, 16, 330, 82), 8, 8);
    painter.setBrush(badge);
    painter.drawRoundedRect(QRect(30, 28, 96, 24), 6, 6);
    painter.setPen(QColor(245, 246, 242));
    painter.drawText(QRect(30, 28, 96, 24), Qt::AlignCenter, quality);
    painter.drawText(
        QPoint(30, 72),
        QString::fromLatin1(view_mode_name(scene_.mode))
    );

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
    if (unfold_ || scene_.mode != ViewMode::globe ||
        event->button() != Qt::LeftButton) {
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
    longitude_deg_ = wrap_longitude(
        longitude_deg_ - static_cast<double>(delta.x()) * 0.35
    );
    latitude_deg_ = std::clamp(
        latitude_deg_ + static_cast<double>(delta.y()) * 0.35,
        -89.5,
        89.5
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
    if (camera_callback_) {
        camera_callback_(longitude_deg_, latitude_deg_, final);
    }
}

}  // namespace aeris::viewer
