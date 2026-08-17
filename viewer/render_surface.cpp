// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "render_surface.hpp"

#include <QFontMetricsF>
#include <QPainterPath>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aeris::viewer {
namespace {

constexpr int kMarginPx = 52;
constexpr double kPi = 3.141592653589793238462643383279502884;

struct LabelCandidate final {
    QString text;
    QPointF anchor;
    QRectF collision_rect;
    double score{0.0};
    std::string stable_id;
};

[[nodiscard]] std::uint64_t stable_hash(const std::string_view value) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const char raw : value) {
        const auto byte = static_cast<unsigned char>(raw);
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

[[nodiscard]] double ring_area2(
    const std::vector<geometry::PlanarPoint>& ring
) noexcept {
    if (ring.size() < 3U) return 0.0;
    double area2 = 0.0;
    for (std::size_t index = 0U; index < ring.size(); ++index) {
        const auto& a = ring[index];
        const auto& b = ring[(index + 1U) % ring.size()];
        area2 += a.x * b.y - b.x * a.y;
    }
    return area2;
}

[[nodiscard]] std::optional<geometry::PlanarPoint> ring_anchor(
    const std::vector<geometry::PlanarPoint>& ring
) noexcept {
    if (ring.size() < 3U) return std::nullopt;

    const double area2 = ring_area2(ring);
    if (std::abs(area2) > 1e-12) {
        double sum_x = 0.0;
        double sum_y = 0.0;
        for (std::size_t index = 0U; index < ring.size(); ++index) {
            const auto& a = ring[index];
            const auto& b = ring[(index + 1U) % ring.size()];
            const double cross = a.x * b.y - b.x * a.y;
            sum_x += (a.x + b.x) * cross;
            sum_y += (a.y + b.y) * cross;
        }
        return geometry::PlanarPoint{
            sum_x / (3.0 * area2),
            sum_y / (3.0 * area2),
        };
    }

    double min_x = ring.front().x;
    double max_x = ring.front().x;
    double min_y = ring.front().y;
    double max_y = ring.front().y;
    for (const auto& point : ring) {
        min_x = std::min(min_x, point.x);
        max_x = std::max(max_x, point.x);
        min_y = std::min(min_y, point.y);
        max_y = std::max(max_y, point.y);
    }
    return geometry::PlanarPoint{
        0.5 * (min_x + max_x),
        0.5 * (min_y + max_y),
    };
}

[[nodiscard]] QRectF ring_device_bounds(
    const std::vector<geometry::PlanarPoint>& ring,
    const QTransform& transform
) {
    if (ring.empty()) return {};
    double min_x = ring.front().x;
    double max_x = ring.front().x;
    double min_y = ring.front().y;
    double max_y = ring.front().y;
    for (const auto& point : ring) {
        min_x = std::min(min_x, point.x);
        max_x = std::max(max_x, point.x);
        min_y = std::min(min_y, point.y);
        max_y = std::max(max_y, point.y);
    }
    const QPointF a = transform.map(QPointF(min_x, min_y));
    const QPointF b = transform.map(QPointF(max_x, max_y));
    return QRectF(a, b).normalized();
}

void draw_country_labels(
    QPainter& painter,
    const SceneData& scene
) {
    if (!scene.political) return;

    const QTransform world_transform = painter.worldTransform();
    const QRectF viewport = painter.viewport();

    QFont font = painter.font();
    font.setPixelSize(scene.mode == ViewMode::globe ? 11 : 12);
    font.setWeight(QFont::DemiBold);
    const QFontMetricsF metrics(font);

    std::vector<LabelCandidate> candidates;
    candidates.reserve(scene.features.size());

    for (const SceneFeature& feature : scene.features) {
        if (feature.label.empty() || feature.fill_rings.empty()) continue;

        const std::vector<geometry::PlanarPoint>* largest = nullptr;
        double largest_area = 0.0;
        for (const auto& ring : feature.fill_rings) {
            const double area = std::abs(ring_area2(ring));
            if (area > largest_area) {
                largest_area = area;
                largest = &ring;
            }
        }
        if (largest == nullptr) continue;

        const QRectF feature_bounds = ring_device_bounds(*largest, world_transform);
        if (feature_bounds.width() < 42.0 || feature_bounds.height() < 18.0) continue;

        const auto world_anchor = ring_anchor(*largest);
        if (!world_anchor) continue;
        const QPointF anchor = world_transform.map(
            QPointF(world_anchor->x, world_anchor->y)
        );
        if (!viewport.adjusted(8.0, 8.0, -8.0, -8.0).contains(anchor)) continue;

        const QString text = QString::fromStdString(feature.label);
        const QRectF text_bounds = metrics.boundingRect(text);
        QRectF collision(
            anchor.x() - 0.5 * text_bounds.width() - 4.0,
            anchor.y() - 0.5 * text_bounds.height() - 2.0,
            text_bounds.width() + 8.0,
            text_bounds.height() + 4.0
        );
        if (!viewport.intersects(collision)) continue;

        candidates.push_back({
            text,
            anchor,
            collision,
            feature_bounds.width() * feature_bounds.height(),
            feature.stable_id,
        });
    }

    std::sort(
        candidates.begin(),
        candidates.end(),
        [](const LabelCandidate& left, const LabelCandidate& right) {
            if (left.score != right.score) return left.score > right.score;
            return left.stable_id < right.stable_id;
        }
    );

    painter.save();
    painter.resetTransform();
    painter.setFont(font);

    std::vector<QRectF> occupied;
    occupied.reserve(32U);
    std::size_t drawn = 0U;
    for (const LabelCandidate& candidate : candidates) {
        if (drawn >= 30U) break;
        const bool collides = std::any_of(
            occupied.begin(),
            occupied.end(),
            [&](const QRectF& used) {
                return used.adjusted(-3.0, -2.0, 3.0, 2.0)
                    .intersects(candidate.collision_rect);
            }
        );
        if (collides) continue;

        const QRectF text_rect = candidate.collision_rect.adjusted(4.0, 2.0, -4.0, -2.0);
        painter.setPen(QColor(19, 21, 24, 220));
        painter.drawText(text_rect.translated(1.0, 1.0), Qt::AlignCenter, candidate.text);
        painter.setPen(QColor(239, 240, 235, 238));
        painter.drawText(text_rect, Qt::AlignCenter, candidate.text);

        occupied.push_back(candidate.collision_rect);
        ++drawn;
    }

    painter.restore();
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
    const double opacity,
    const LayerRenderState& layers
) {
    if (opacity <= 0.0) return;

    painter.save();
    painter.setOpacity(std::clamp(opacity, 0.0, 1.0));

    if (scene.mode == ViewMode::globe && scene.globe_radius_m > 0.0) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(scene.political ? QColor(37, 44, 53) : QColor(42, 48, 56));
        painter.drawEllipse(QPointF(0.0, 0.0), scene.globe_radius_m, scene.globe_radius_m);
    }

    if (layers.fill_visible) {
        painter.setPen(Qt::NoPen);
        for (const SceneFeature& feature : scene.features) {
            if (feature.fill_rings.empty()) continue;
            painter.setBrush(scene.political ? political_fill(feature) : QColor(197, 198, 188));
            painter.drawPath(fill_path(feature));
        }
    }

    if (layers.outline_visible) {
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
    }

    if (scene.mode == ViewMode::globe && scene.globe_radius_m > 0.0) {
        QPen limb(scene.political ? QColor(137, 151, 166) : QColor(122, 132, 146));
        limb.setWidthF(1.25);
        limb.setCosmetic(true);
        painter.setPen(limb);
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(QPointF(0.0, 0.0), scene.globe_radius_m, scene.globe_radius_m);
    }

    if (layers.labels_visible) draw_country_labels(painter, scene);

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
