// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "unfold_controller.hpp"

#include <QMetaObject>
#include <QPointer>
#include <QRunnable>

#include <utility>

namespace aeris::viewer {
namespace {

class UnfoldTask final : public QRunnable {
public:
    UnfoldTask(
        QPointer<UnfoldController> target,
        std::shared_ptr<const source::Result> world,
        const double camera_longitude_deg,
        const double camera_latitude_deg,
        const ViewMode target_mode,
        std::shared_ptr<std::atomic_bool> canceled,
        const std::uint64_t generation
    )
        : target_(std::move(target)),
          world_(std::move(world)),
          camera_longitude_deg_(camera_longitude_deg),
          camera_latitude_deg_(camera_latitude_deg),
          target_mode_(target_mode),
          canceled_(std::move(canceled)),
          generation_(generation) {
        setAutoDelete(true);
    }

    void run() override {
        const auto token = canceled_;
        UnfoldBundle bundle = build_unfold_bundle(
            *world_,
            camera_longitude_deg_,
            camera_latitude_deg_,
            target_mode_,
            [token]() { return token->load(std::memory_order_relaxed); }
        );

        QPointer<UnfoldController> target = target_;
        QMetaObject::invokeMethod(
            target_,
            [target, generation = generation_, bundle = std::move(bundle)]() mutable {
                if (target) {
                    target->accept_background_bundle(
                        generation,
                        std::move(bundle)
                    );
                }
            },
            Qt::QueuedConnection
        );
    }

private:
    QPointer<UnfoldController> target_;
    std::shared_ptr<const source::Result> world_;
    double camera_longitude_deg_ = 0.0;
    double camera_latitude_deg_ = 0.0;
    ViewMode target_mode_ = ViewMode::mollweide;
    std::shared_ptr<std::atomic_bool> canceled_;
    std::uint64_t generation_ = 0U;
};

}  // namespace

UnfoldController::UnfoldController(
    std::shared_ptr<const source::Result> world,
    QObject* parent
)
    : QObject(parent), world_(std::move(world)) {
    pool_.setMaxThreadCount(1);
}

UnfoldController::~UnfoldController() {
    cancel();
    pool_.waitForDone();
}

void UnfoldController::set_bundle_callback(BundleCallback callback) {
    bundle_callback_ = std::move(callback);
}

void UnfoldController::set_busy_callback(BusyCallback callback) {
    busy_callback_ = std::move(callback);
}

void UnfoldController::cancel() {
    if (cancel_token_) {
        cancel_token_->store(true, std::memory_order_relaxed);
    }
    ++generation_;
}

void UnfoldController::request(
    const double camera_longitude_deg,
    const double camera_latitude_deg,
    const ViewMode target_mode
) {
    cancel();
    const std::uint64_t generation = generation_;
    cancel_token_ = std::make_shared<std::atomic_bool>(false);

    if (busy_callback_) {
        busy_callback_(true);
    }

    pool_.start(new UnfoldTask(
        QPointer<UnfoldController>(this),
        world_,
        camera_longitude_deg,
        camera_latitude_deg,
        target_mode,
        cancel_token_,
        generation
    ));
}

void UnfoldController::accept_background_bundle(
    const std::uint64_t generation,
    UnfoldBundle bundle
) {
    if (generation != generation_ || bundle.canceled) {
        return;
    }
    if (busy_callback_) {
        busy_callback_(false);
    }
    if (bundle_callback_) {
        bundle_callback_(std::move(bundle));
    }
}

}  // namespace aeris::viewer
