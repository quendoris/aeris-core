// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/storage/project.hpp"

#include <filesystem>
#include <iostream>

int main() {
    using namespace aeris::storage;

    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        std::filesystem::path(u8"aeris-\u0442\u0435\u0441\u0442-utf8-path");
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    ec.clear();
    std::filesystem::create_directories(root, ec);
    if (ec) {
        std::cerr << "unable to create UTF-8 test directory\n";
        return 1;
    }

    const std::filesystem::path project_path =
        root / std::filesystem::path(u8"\u043a\u0430\u0440\u0442\u0430.aeris");

    ProjectCreateOptions options{};
    options.timestamp_utc = "2026-09-02T21:00:00Z";
    options.project_uuid = "6f5fc84a-971d-4d16-a823-6dc1f68ce5ec";
    options.producer = "aeris-test";
    options.producer_version = "0.1.0-test";

    auto created = ProjectStore::create(project_path, options);
    if (!created.ok()) {
        std::cerr << "UTF-8 project create failed: " << created.status.diagnostic << '\n';
        return 1;
    }
    created.store.reset();

    auto reopened = ProjectStore::open(project_path);
    if (!reopened.ok()) {
        std::cerr << "UTF-8 project reopen failed: " << reopened.status.diagnostic << '\n';
        return 1;
    }
    if (reopened.store->metadata().project_uuid != options.project_uuid) {
        std::cerr << "UTF-8 project identity changed after reopen\n";
        return 1;
    }
    reopened.store.reset();

    std::filesystem::remove_all(root, ec);
    return 0;
}
