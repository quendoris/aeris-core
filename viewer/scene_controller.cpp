// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "scene_controller.hpp"

#include "scene_builder.hpp"

#include <QMetaObject>
#include <QPointer>
#include <QRunnable>

#include <utility>

namespace aeris::viewer {
namespace {

class SceneTask final : public QRunnable {
public:
    SceneTask(
        QPointer<SceneController> target,
        std::shared_ptr<const source::Result> world,
        SceneRequest request,
        std::shared_ptr<std::atomic_bool> canceled,
        const std::uint64_t generation
    )
        : target_(std::move(target)),
          world_(std::move(world)),
          request_(request),
          canceled_(std::move(canceled)),
          generation_(generation) {
        setAutoDelete(true);
    }

    void run() override {
        const auto token = canceled_;
        SceneData scene = build_scene(
            *world_,
            request_,
            [token]() { return token->load(std::memory_order_relaxed); }
        );

        QPointer<SceneController> target = target_;
        QMetaObject::invokeMethod(
            target_,
            [target, generation = generation_, scene = std::move(scene)]() mutable {
                if (target) {
                    target->accept_background_scene(
                        generation,
                        std::move(scene)
                    );
                }
            },
            Qt::QueuedConnection
        );
    }

private:
    QPointer<SceneController> target_;
    std::shared_ptr<const source::Result> world_;
    SceneRequest request_{};
    std::shared_ptr<std::atomic_bool> canceled_;
    std::uint64_t generation_ = 0U;
};

}  // namespace

SceneController::SceneController(
    std::shared_ptr<const source::Result> world,
    QObject* parent
)
    : QObject(parent), world_(std::move(world)) {
    pool_.setMaxThreadCount(1);
}

SceneController::~SceneController() {
    cancel();
    pool_.waitForDone();
}

void SceneController::set_scene_callback(SceneCallback callback) {
    scene_callback_ = std::move(callback);
}

void SceneController::set_busy_callback(BusyCallback callback) {
    busy_callback_ = std::move(callback);
}

void SceneController::cancel() {
    if (cancel_token_) {
        cancel_token_->store(true, std::memory_order_relaxed);
    }
    ++generation_;
}

void SceneController::request_preview(const SceneRequest& request) {
    cancel();
    if (busy_callback_) {
        busy_callback_(false);
    }

    SceneData scene = build_scene(*world_, request);
    if (scene_callback_) {
        scene_callback_(std::move(scene));
    }
}

void SceneController::request_verified(const SceneRequest& request) {
    cancel();
    const std::uint64_t generation = generation_;
    cancel_token_ = std::make_shared<std::atomic_bool>(false);

    if (busy_callback_) {
        busy_callback_(true);
    }

    pool_.start(new SceneTask(
        QPointer<SceneController>(this),
        world_,
        request,
        cancel_token_,
        generation
    ));
}

void SceneController::accept_background_scene(
    const std::uint64_t generation,
    SceneData scene
) {
    if (generation != generation_ || scene.canceled) {
        return;
    }
    if (busy_callback_) {
        busy_callback_(false);
    }
    if (scene_callback_) {
        scene_callback_(std::move(scene));
    }
}

}  // namespace aeris::viewer
