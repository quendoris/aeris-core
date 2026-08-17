// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "aeris/source/adapter.hpp"
#include "scene.hpp"

#include <QObject>
#include <QThreadPool>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

namespace aeris::viewer {

class SceneController final : public QObject {
public:
    using SceneCallback = std::function<void(SceneData)>;
    using BusyCallback = std::function<void(bool)>;

    explicit SceneController(
        std::shared_ptr<const source::Result> world,
        QObject* parent = nullptr
    );
    ~SceneController() override;

    void set_scene_callback(SceneCallback callback);
    void set_busy_callback(BusyCallback callback);
    void set_world(std::shared_ptr<const source::Result> world);

    void request_preview(const SceneRequest& request);
    void request_verified(const SceneRequest& request);
    void cancel();

    void accept_background_scene(std::uint64_t generation, SceneData scene);

private:
    std::shared_ptr<const source::Result> world_;
    QThreadPool pool_;
    std::shared_ptr<std::atomic_bool> cancel_token_;
    std::uint64_t generation_ = 0U;
    SceneCallback scene_callback_;
    BusyCallback busy_callback_;
};

}  // namespace aeris::viewer
