// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#include "aeris/storage/project.hpp"
#include "aeris/storage/session.hpp"

#include <atomic>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

namespace {

struct CreateOutcome {
    bool ok{false};
    aeris::storage::StorageError error{aeris::storage::StorageError::none};
    std::string project_uuid;
};

bool has_staging_directory(const std::filesystem::path& root, const std::string& prefix) {
    std::error_code ec;
    for (std::filesystem::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().filename().string().rfind(prefix, 0U) == 0U) return true;
    }
    return false;
}

void wait_for_start(std::atomic<int>& ready, std::atomic<bool>& go) {
    ready.fetch_add(1, std::memory_order_release);
    while (!go.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}

}  // namespace

int main() {
    using namespace aeris::storage;

    const std::filesystem::path root = std::filesystem::temp_directory_path() / "aeris-storage-race-test";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    if (ec) return 10;

    const std::filesystem::path contested_path = root / "contested.aeris";
    const std::string uuid_a = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
    const std::string uuid_b = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";

    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    CreateOutcome outcomes[2];

    const auto project_worker = [&](const int index, const std::string& uuid) {
        ProjectCreateOptions options;
        options.timestamp_utc = "2026-08-16T19:30:00Z";
        options.project_uuid = uuid;
        wait_for_start(ready, go);
        auto result = ProjectStore::create(contested_path, options);
        outcomes[index].ok = result.ok();
        outcomes[index].error = result.status.error;
        if (result.ok()) outcomes[index].project_uuid = result.store->metadata().project_uuid;
    };

    std::thread first(project_worker, 0, std::cref(uuid_a));
    std::thread second(project_worker, 1, std::cref(uuid_b));
    while (ready.load(std::memory_order_acquire) != 2) std::this_thread::yield();
    go.store(true, std::memory_order_release);
    first.join();
    second.join();

    const int success_count = static_cast<int>(outcomes[0].ok) + static_cast<int>(outcomes[1].ok);
    const int exists_count = static_cast<int>(outcomes[0].error == StorageError::path_exists) +
                             static_cast<int>(outcomes[1].error == StorageError::path_exists);
    if (success_count != 1 || exists_count != 1) {
        std::cerr << "project race expected one success and one path_exists; outcomes="
                  << outcomes[0].ok << '/' << static_cast<int>(outcomes[0].error) << ','
                  << outcomes[1].ok << '/' << static_cast<int>(outcomes[1].error) << '\n';
        return 11;
    }
    if (has_staging_directory(root, ".aeris-create-")) return 12;

    auto project = ProjectStore::open(contested_path);
    if (!project.ok()) {
        std::cerr << "winning project did not reopen: " << project.status.diagnostic << '\n';
        return 13;
    }
    const std::string winning_uuid = project.store->metadata().project_uuid;
    if (winning_uuid != uuid_a && winning_uuid != uuid_b) return 14;
    if ((outcomes[0].ok && winning_uuid != uuid_a) || (outcomes[1].ok && winning_uuid != uuid_b)) return 15;

    ready.store(0, std::memory_order_release);
    go.store(false, std::memory_order_release);
    bool session_ok[2] = {false, false};
    StorageError session_error[2] = {StorageError::none, StorageError::none};

    const auto session_worker = [&](const int index, const char* timestamp) {
        wait_for_start(ready, go);
        auto session = SessionStore::open_or_create(*project.store, timestamp);
        session_ok[index] = session.ok();
        session_error[index] = session.status.error;
        if (session.ok() && session.store->metadata().project_uuid != winning_uuid) {
            session_ok[index] = false;
            session_error[index] = StorageError::session_project_mismatch;
        }
    };

    std::thread session_first(session_worker, 0, "2026-08-16T19:30:01Z");
    std::thread session_second(session_worker, 1, "2026-08-16T19:30:02Z");
    while (ready.load(std::memory_order_acquire) != 2) std::this_thread::yield();
    go.store(true, std::memory_order_release);
    session_first.join();
    session_second.join();

    if (!session_ok[0] || !session_ok[1]) {
        std::cerr << "session race expected both callers to accept the one published winner; errors="
                  << static_cast<int>(session_error[0]) << ',' << static_cast<int>(session_error[1]) << '\n';
        return 16;
    }
    if (has_staging_directory(root, ".aeris-session-create-")) return 17;

    auto session = SessionStore::open_or_create(*project.store, "2026-08-16T19:30:03Z");
    if (!session.ok()) {
        std::cerr << "session winner did not reopen: " << session.status.diagnostic << '\n';
        return 18;
    }
    if (session.store->metadata().project_uuid != winning_uuid || session.store->metadata().revision != 0U) return 19;
    auto view = session.store->read_view_state();
    if (!view.ok() || view.value.has_value()) return 20;

    session.store.reset();
    project.store.reset();
    std::filesystem::remove_all(root, ec);
    return ec ? 21 : 0;
}
