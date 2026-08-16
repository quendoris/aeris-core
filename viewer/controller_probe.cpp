// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "scene_controller.hpp"
#include "unfold_controller.hpp"
#include "world_loader.hpp"

#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

[[nodiscard]] bool near(const double left, const double right) noexcept {
    return std::abs(left - right) <= 1e-12;
}

[[nodiscard]] bool run_scene_lifecycle(
    const std::shared_ptr<const aeris::source::Result>& world
) {
    aeris::viewer::SceneController controller(world);
    QEventLoop wait_for_final;
    QTimer timeout;
    timeout.setSingleShot(true);

    bool timed_out = false;
    bool stale_scene_delivered = false;
    bool preview_seen = false;
    bool final_verified_seen = false;
    bool busy_seen = false;
    bool ready_after_busy_seen = false;
    int preview_callbacks = 0;
    int verified_callbacks = 0;

    controller.set_busy_callback([&](const bool busy) {
        if (busy) {
            busy_seen = true;
        } else if (busy_seen) {
            ready_after_busy_seen = true;
        }
    });

    controller.set_scene_callback([&](aeris::viewer::SceneData scene) {
        if (scene.quality == aeris::viewer::SceneQuality::preview) {
            ++preview_callbacks;
            if (scene.ok &&
                scene.mode == aeris::viewer::ViewMode::globe &&
                near(scene.camera_longitude_deg, 45.0) &&
                near(scene.camera_latitude_deg, 10.0) &&
                scene.fill_rings == 0U &&
                scene.outline_parts > 0U) {
                preview_seen = true;
            } else {
                stale_scene_delivered = true;
            }
            return;
        }

        ++verified_callbacks;
        if (scene.ok &&
            scene.mode == aeris::viewer::ViewMode::globe &&
            near(scene.camera_longitude_deg, 60.0) &&
            near(scene.camera_latitude_deg, 30.0) &&
            scene.fill_rings > 0U &&
            scene.outline_parts > 0U &&
            scene.max_refinement_rounds >= 2U) {
            final_verified_seen = true;
            wait_for_final.quit();
        } else {
            stale_scene_delivered = true;
        }
    });

    QObject::connect(&timeout, &QTimer::timeout, [&]() {
        timed_out = true;
        wait_for_final.quit();
    });

    aeris::viewer::SceneRequest stale{};
    stale.mode = aeris::viewer::ViewMode::globe;
    stale.quality = aeris::viewer::SceneQuality::verified;
    stale.camera_longitude_deg = 15.0;
    stale.camera_latitude_deg = 20.0;
    controller.request_verified(stale);

    aeris::viewer::SceneRequest preview{};
    preview.mode = aeris::viewer::ViewMode::globe;
    preview.quality = aeris::viewer::SceneQuality::preview;
    preview.camera_longitude_deg = 45.0;
    preview.camera_latitude_deg = 10.0;
    controller.request_preview(preview);

    aeris::viewer::SceneRequest final{};
    final.mode = aeris::viewer::ViewMode::globe;
    final.quality = aeris::viewer::SceneQuality::verified;
    final.camera_longitude_deg = 60.0;
    final.camera_latitude_deg = 30.0;
    controller.request_verified(final);

    timeout.start(120'000);
    wait_for_final.exec();
    timeout.stop();
    controller.cancel();

    if (timed_out || stale_scene_delivered ||
        !preview_seen || !final_verified_seen ||
        preview_callbacks != 1 || verified_callbacks != 1 ||
        !busy_seen || !ready_after_busy_seen) {
        std::cerr
            << "scene controller lifecycle failed: timeout=" << timed_out
            << " stale=" << stale_scene_delivered
            << " preview_seen=" << preview_seen
            << " final_verified_seen=" << final_verified_seen
            << " preview_callbacks=" << preview_callbacks
            << " verified_callbacks=" << verified_callbacks
            << " busy_seen=" << busy_seen
            << " ready_after_busy=" << ready_after_busy_seen
            << '\n';
        return false;
    }
    return true;
}

[[nodiscard]] bool run_unfold_lifecycle(
    const std::shared_ptr<const aeris::source::Result>& world
) {
    aeris::viewer::UnfoldController controller(world);
    QEventLoop wait_for_final;
    QTimer timeout;
    timeout.setSingleShot(true);

    bool timed_out = false;
    bool stale_bundle_delivered = false;
    bool final_bundle_seen = false;
    bool busy_seen = false;
    bool ready_after_busy_seen = false;
    int bundle_callbacks = 0;

    controller.set_busy_callback([&](const bool busy) {
        if (busy) {
            busy_seen = true;
        } else if (busy_seen) {
            ready_after_busy_seen = true;
        }
    });
    controller.set_bundle_callback([&](aeris::viewer::UnfoldBundle bundle) {
        ++bundle_callbacks;
        const bool expected =
            bundle.ok && !bundle.canceled &&
            bundle.target_mode == aeris::viewer::ViewMode::sinusoidal &&
            bundle.guides.size() == 24U &&
            bundle.globe_endpoint.quality == aeris::viewer::SceneQuality::verified &&
            bundle.flat_endpoint.quality == aeris::viewer::SceneQuality::verified &&
            near(bundle.globe_endpoint.camera_longitude_deg, 45.0) &&
            near(bundle.globe_endpoint.camera_latitude_deg, 10.0);

        if (expected) {
            final_bundle_seen = true;
            wait_for_final.quit();
            return;
        }

        stale_bundle_delivered = true;
        std::cerr
            << "unexpected unfold bundle:"
            << " ok=" << bundle.ok
            << " canceled=" << bundle.canceled
            << " target=" << static_cast<unsigned>(bundle.target_mode)
            << " guides=" << bundle.guides.size()
            << " globe_quality=" << static_cast<unsigned>(bundle.globe_endpoint.quality)
            << " flat_quality=" << static_cast<unsigned>(bundle.flat_endpoint.quality)
            << " globe_ok=" << bundle.globe_endpoint.ok
            << " flat_ok=" << bundle.flat_endpoint.ok
            << " globe_camera=" << bundle.globe_endpoint.camera_longitude_deg
            << ',' << bundle.globe_endpoint.camera_latitude_deg
            << " flat_camera=" << bundle.flat_endpoint.camera_longitude_deg
            << ',' << bundle.flat_endpoint.camera_latitude_deg
            << " diagnostic=" << bundle.diagnostic
            << '\n';
        wait_for_final.quit();
    });
    QObject::connect(&timeout, &QTimer::timeout, [&]() {
        timed_out = true;
        wait_for_final.quit();
    });

    // Generation A must be canceled before it can become presentation state.
    controller.request(15.0, 20.0, aeris::viewer::ViewMode::mollweide);
    // Generation B is the only unfold bundle allowed through the callback.
    controller.request(45.0, 10.0, aeris::viewer::ViewMode::sinusoidal);

    timeout.start(180'000);
    wait_for_final.exec();
    timeout.stop();
    controller.cancel();

    if (timed_out || stale_bundle_delivered || !final_bundle_seen ||
        bundle_callbacks != 1 || !busy_seen || !ready_after_busy_seen) {
        std::cerr
            << "unfold controller lifecycle failed: timeout=" << timed_out
            << " stale=" << stale_bundle_delivered
            << " final_bundle_seen=" << final_bundle_seen
            << " callbacks=" << bundle_callbacks
            << " busy_seen=" << busy_seen
            << " ready_after_busy=" << ready_after_busy_seen
            << '\n';
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::filesystem::path snapshot =
        std::filesystem::path("dev-data") / "natural-earth-v5.1.2";
    if (argc == 3 && std::string(argv[1]) == "--snapshot") {
        snapshot = argv[2];
    } else if (argc != 1) {
        std::cerr << "usage: aeris_viewer_controller_probe [--snapshot <directory>]\n";
        return EXIT_FAILURE;
    }

    QCoreApplication application(argc, argv);

    auto loaded = aeris::viewer::load_pinned_demo_world(
        snapshot,
        "viewer-controller-probe"
    );
    if (!loaded.ok()) {
        std::cerr << "controller probe source load failed: "
                  << loaded.diagnostic << '\n';
        return EXIT_FAILURE;
    }

    if (!run_scene_lifecycle(loaded.world) ||
        !run_unfold_lifecycle(loaded.world)) {
        return EXIT_FAILURE;
    }

    std::cout
        << "viewer_controller_probe: PASS\n"
        << "scene stale generation delivered: no\n"
        << "unfold stale generation delivered: no\n";
    return EXIT_SUCCESS;
}
