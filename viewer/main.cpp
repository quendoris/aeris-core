// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "main_window.hpp"
#include "scene_builder.hpp"
#include "world_loader.hpp"

#include <QApplication>
#include <QDateTime>
#include <QImage>
#include <QMessageBox>
#include <QPainter>
#include <QTimer>

#include <filesystem>
#include <iostream>
#include <string>

namespace {

struct Arguments final {
    std::filesystem::path snapshot =
        std::filesystem::path("dev-data") / "natural-earth-v5.1.2";
    std::filesystem::path render_path;
    bool smoke = false;
    bool render = false;
    bool help = false;
    bool valid = true;
};

[[nodiscard]] Arguments parse_arguments(const int argc, char** argv) {
    Arguments arguments{};
    for (int index = 1; index < argc; ++index) {
        const std::string value(argv[index]);
        if (value == "--snapshot") {
            if (index + 1 >= argc) {
                arguments.valid = false;
                return arguments;
            }
            arguments.snapshot = argv[++index];
        } else if (value == "--smoke") {
            arguments.smoke = true;
        } else if (value == "--render") {
            if (index + 1 >= argc) {
                arguments.valid = false;
                return arguments;
            }
            arguments.render = true;
            arguments.render_path = argv[++index];
        } else if (value == "--help" || value == "-h") {
            arguments.help = true;
        } else {
            arguments.valid = false;
            return arguments;
        }
    }
    if (arguments.smoke && arguments.render) {
        arguments.valid = false;
    }
    return arguments;
}

void print_usage() {
    std::cout
        << "usage: aeris_viewer [--snapshot <directory>] [--smoke | --render <png>]\n"
        << "\n"
        << "The default snapshot directory is dev-data/natural-earth-v5.1.2.\n"
        << "Fetch the exact pinned demo bytes with:\n"
        << "  cmake -DDESTINATION=dev-data/natural-earth-v5.1.2 "
           "-P scripts/fetch_demo_world.cmake\n";
}

[[nodiscard]] bool render_verified_workbench(
    QApplication& application,
    const std::shared_ptr<const aeris::source::Result>& world,
    const std::filesystem::path& output_path
) {
    aeris::viewer::SceneRequest request{};
    request.mode = aeris::viewer::ViewMode::globe;
    request.quality = aeris::viewer::SceneQuality::verified;
    request.camera_longitude_deg = 15.0;
    request.camera_latitude_deg = 20.0;

    aeris::viewer::SceneData scene = aeris::viewer::build_scene(*world, request);
    if (!scene.ok || scene.canceled) {
        std::cerr << "viewer render scene failed: " << scene.diagnostic << '\n';
        return false;
    }

    aeris::viewer::MainWindow window(world, false);
    window.present_scene(std::move(scene));
    window.resize(1280, 820);
    window.show();
    application.processEvents();

    QImage image(window.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    window.render(&painter);
    painter.end();

    return image.save(
        QString::fromStdString(output_path.string()),
        "PNG"
    );
}

}  // namespace

int main(int argc, char** argv) {
    const Arguments arguments = parse_arguments(argc, argv);
    if (!arguments.valid) {
        print_usage();
        return EXIT_FAILURE;
    }
    if (arguments.help) {
        print_usage();
        return EXIT_SUCCESS;
    }

    QApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("AERIS"));
    application.setOrganizationName(QStringLiteral("quendoris"));

    const std::string retrieved_at =
        QDateTime::currentDateTimeUtc()
            .toString(Qt::ISODateWithMs)
            .toStdString();
    auto loaded = aeris::viewer::load_pinned_demo_world(
        arguments.snapshot,
        retrieved_at
    );
    if (!loaded.ok()) {
        const QString message = QStringLiteral(
            "AERIS could not verify the pinned demo world at:\n%1\n\n%2\n\n"
            "Run the explicit demo-data fetch command shown by --help."
        )
            .arg(QString::fromStdString(arguments.snapshot.string()))
            .arg(QString::fromStdString(loaded.diagnostic));
        if (arguments.smoke || arguments.render) {
            std::cerr << message.toStdString() << '\n';
        } else {
            QMessageBox::critical(nullptr, QStringLiteral("AERIS source verification"), message);
        }
        return EXIT_FAILURE;
    }

    if (arguments.render) {
        if (!render_verified_workbench(
                application,
                loaded.world,
                arguments.render_path
            )) {
            std::cerr << "unable to render viewer PNG\n";
            return EXIT_FAILURE;
        }
        std::cout << "viewer_render: PASS "
                  << arguments.render_path.string() << '\n';
        return EXIT_SUCCESS;
    }

    aeris::viewer::MainWindow window(loaded.world, !arguments.smoke);
    window.show();

    if (arguments.smoke) {
        QTimer::singleShot(0, &application, &QCoreApplication::quit);
    }

    return application.exec();
}
