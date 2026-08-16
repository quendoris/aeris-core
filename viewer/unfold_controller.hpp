// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "aeris/source/adapter.hpp"
#include "unfold.hpp"

#include <QObject>
#include <QThreadPool>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

namespace aeris::viewer {

class UnfoldController final : public QObject {
public:
    using BundleCallback = std::function<void(UnfoldBundle)>;
    using BusyCallback = std::function<void(bool)>;

    explicit UnfoldController(
        std::shared_ptr<const source::Result> world,
        QObject* parent = nullptr
    );
    ~UnfoldController() override;

    void set_bundle_callback(BundleCallback callback);
    void set_busy_callback(BusyCallback callback);

    void request(
        double camera_longitude_deg,
        double camera_latitude_deg,
        ViewMode target_mode
    );
    void cancel();

    void accept_background_bundle(
        std::uint64_t generation,
        UnfoldBundle bundle
    );

private:
    std::shared_ptr<const source::Result> world_;
    QThreadPool pool_;
    BundleCallback bundle_callback_;
    BusyCallback busy_callback_;
    std::shared_ptr<std::atomic_bool> cancel_token_;
    std::uint64_t generation_ = 0U;
};

}  // namespace aeris::viewer
