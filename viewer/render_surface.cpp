// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "render_surface.hpp"

#include <QPainterPath>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace aeris::viewer {
namespace {

constexpr int kMarginPx = 52;
constexpr double kPi = 3.141592653589793238462643383279502884;

[[nodiscard]] std::uint64_t stable_hash(const std::string_view value) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char byte : value) {
        hash ^= static_cast<std::uint64_t>(byte);
        hash *= 1099511628211ULL;
    }
    return hash;
}

[[nodiscard]] QColor political_fill(const SceneFeature& feature) {
    static const std::array<QColor, 10> palette{{
        QColor(112, 145, 166),
        QColor(151, 126, 164),
        QColor(126, 157, 127),
        QColor(171, 139, 104),
        QColor(111, 154, 151),
        QColor(158, 118, 123),
        QColor(143, 149, 105),
        QColor(112, 129, 168),
        QColor(158, 130, 102),
        QColor(121, 147, 137),
    }};
    const std::string_view key = feature.style_key.empty()
        ? std::string_view(feature.stable_id)
        : std::string_view(feature.style_key);
    return palette[static_cast<std::size_t>(stable_hash(key) % palette.size())];
}

[[nodiscard]] QPainterPath fill_path(const SceneFeature& feature) {
    QPainterPath path;
    path.setFillRule(Qt::OddEvenFill);
    for (const auto& ring : feature.fill_rings) {
        if (ring.size() < 3U) continue;
        path.moveTo(ring.front().x, ring.front().y);
        for (std::size_t index = 1U; index < ring.size(); ++index) {
            path.lineTo(ring[index].x, ring[index].y);
        }
        path.closeSubpath();
    }
    return path;
}

}  // namespace

CanvasBounds scene_bounds(const SceneData& scene) noexcept {
    return {scene.min_x, scene.min_y, scene.max_x, scene.max_y};
}

CanvasBounds interpolate_bounds(
    const CanvasBounds& from,
    const CanvasBounds& to,
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
    const CanvasBounds& bounds,
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
    if (opacity <= 0.0) return;

    painter.save();
    painter.setOpacity(std::clamp(opacity, 0.0, 1.0));

    if (scene.mode == ViewMode::globe && scene.globe_radius_m > 0.0) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(scene.political ? QColor(37, 44, 53) : QColor(42, 48, 56));
        painter.drawEllipse(QPointF(0.0, 0.0), scene.globe_radius_m, scene.globe_radius_m);
    }

    painter.setPen(Qt::NoPen);
    for (const SceneFeature& feature : scene.features) {
        if (feature.fill_rings.empty()) continue;
        painter.setBrush(scene.political ? political_fill(feature) : QColor(197, 198, 188));
        painter.drawPath(fill_path(feature));
    }

    QPen outline(scene.political ? QColor(41, 44, 48, 225) : QColor(230, 231, 226));
    outline.setWidthF(scene.political ? 0.85 : 1.0);
    outline.setCosmetic(true);
    outline.setJoinStyle(Qt::RoundJoin);
    outline.setCapStyle(Qt::RoundCap);
    painter.setPen(outline);
    painter.setBrush(Qt::NoBrush);
    for (const SceneFeature& feature : scene.features) {
        for (const auto& line : feature.outlines) {
            if (line.size() < 2U) continue;
            QPainterPath path;
            path.moveTo(line.front().x, line.front().y);
            for (std::size_t index = 1U; index < line.size(); ++index) {
                path.lineTo(line[index].x, line[index].y);
            }
            if (scene.mode != ViewMode::globe && line.size() >= 3U) path.closeSubpath();
            painter.drawPath(path);
        }
    }

    if (scene.mode == ViewMode::globe && scene.globe_radius_m > 0.0) {
        QPen limb(scene.political ? QColor(137, 151, 166) : QColor(122, 132, 146));
        limb.setWidthF(1.25);
        limb.setCosmetic(true);
        painter.setPen(limb);
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(QPointF(0.0, 0.0), scene.globe_radius_m, scene.globe_radius_m);
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
    if (transition_alpha <= 0.0) return;

    for (const auto& line : bundle.guides) {
        if (line.vertices.size() < 2U) continue;
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
            if (alpha <= 0.0) continue;

            const auto pa = interpolate_unfold_vertex(a, progress);
            const auto pb = interpolate_unfold_vertex(b, progress);
            painter.save();
            painter.setOpacity(std::clamp(alpha, 0.0, 1.0));
            painter.drawLine(QPointF(pa.x, pa.y), QPointF(pb.x, pb.y));
            painter.restore();
        }
    }
}

QString scene_caption(const SceneData& scene) {
    const QString mode = QString::fromLatin1(view_mode_name(scene.mode));
    return scene.political ? QStringLiteral("Political · %1").arg(mode) : mode;
}

}  // namespace aeris::viewer
