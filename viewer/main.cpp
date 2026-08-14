// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "main_window.hpp"
#include "world_loader.hpp"

#include <QApplication>
#include <QDateTime>
#include <QMessageBox>
#include <QTimer>

#include <filesystem>
#include <iostream>
#include <string>

namespace {

struct Arguments final {
    std::filesystem::path snapshot =
        std::filesystem::path("dev-data") / "natural-earth-v5.1.2";
    bool smoke = false;
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
        } else if (value == "--help" || value == "-h") {
            arguments.help = true;
        } else {
            arguments.valid = false;
            return arguments;
        }
    }
    return arguments;
}

void print_usage() {
    std::cout
        << "usage: aeris_viewer [--snapshot <directory>] [--smoke]\n"
        << "\n"
        << "The default snapshot directory is dev-data/natural-earth-v5.1.2.\n"
        << "Fetch the exact pinned demo bytes with:\n"
        << "  cmake -DDESTINATION=dev-data/natural-earth-v5.1.2 "
           "-P scripts/fetch_demo_world.cmake\n";
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
        if (arguments.smoke) {
            std::cerr << message.toStdString() << '\n';
        } else {
            QMessageBox::critical(nullptr, QStringLiteral("AERIS source verification"), message);
        }
        return EXIT_FAILURE;
    }

    aeris::viewer::MainWindow window(loaded.world, !arguments.smoke);
    window.show();

    if (arguments.smoke) {
        QTimer::singleShot(0, &application, &QCoreApplication::quit);
    }

    return application.exec();
}
