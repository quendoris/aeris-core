// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#include "aeris/storage/project.hpp"
#include "aeris/storage/session.hpp"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

constexpr const char* kUuid = "12345678-90ab-4cde-8f01-23456789abcd";

int write_then_abort(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::remove_all(path.parent_path(), ec);
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return 10;

    aeris::storage::ProjectCreateOptions options;
    options.timestamp_utc = "2026-08-16T18:20:00Z";
    options.project_uuid = kUuid;
    auto project = aeris::storage::ProjectStore::create(path, options);
    if (!project.ok()) return 11;

    aeris::storage::ProjectMetadataUpdate mutation;
    mutation.modified_utc = "2026-08-16T18:20:01Z";
    mutation.projection_id = "aeris.projection.sinusoidal.v1";
    if (!project.store->update_metadata(mutation).ok()) return 12;

    auto session = aeris::storage::SessionStore::open_or_create(*project.store, "2026-08-16T18:20:02Z");
    if (!session.ok()) return 13;
    aeris::storage::SessionViewState view;
    view.mode = aeris::storage::SessionViewState::Mode::mollweide;
    view.camera_longitude_rad = 0.75;
    view.camera_latitude_rad = -0.25;
    view.zoom = 2.5;
    if (!session.store->write_view_state(view, "2026-08-16T18:20:03Z").ok()) return 14;

    // Deliberately skip all C++ destructors and sqlite3_close_v2(). A successful
    // verifier proves acknowledged commits do not depend on graceful teardown.
    std::_Exit(0);
}

int verify_after_abort(const std::filesystem::path& path) {
    auto project = aeris::storage::ProjectStore::open(path);
    if (!project.ok()) {
        std::cerr << "project reopen failed: " << project.status.diagnostic << '\n';
        return 20;
    }
    if (project.store->metadata().revision != 1U ||
        project.store->metadata().projection_id != "aeris.projection.sinusoidal.v1") {
        return 21;
    }

    auto session = aeris::storage::SessionStore::open_or_create(*project.store, "2026-08-16T18:20:04Z");
    if (!session.ok()) {
        std::cerr << "session reopen failed: " << session.status.diagnostic << '\n';
        return 22;
    }
    if (session.store->metadata().revision != 1U) return 23;
    auto view = session.store->read_view_state();
    if (!view.ok() || !view.value) return 24;
    if (view.value->mode != aeris::storage::SessionViewState::Mode::mollweide ||
        std::abs(view.value->camera_longitude_rad - 0.75) > 1e-15 ||
        std::abs(view.value->camera_latitude_rad + 0.25) > 1e-15 ||
        std::abs(view.value->zoom - 2.5) > 1e-15) {
        return 25;
    }

    std::error_code ec;
    project.store.reset();
    session.store.reset();
    std::filesystem::remove_all(path.parent_path(), ec);
    return ec ? 26 : 0;
}

}  // namespace

int main(const int argc, char** argv) {
    if (argc != 3) return 2;
    const std::string mode = argv[1];
    const std::filesystem::path path = argv[2];
    if (mode == "write") return write_then_abort(path);
    if (mode == "verify") return verify_after_abort(path);
    return 3;
}
