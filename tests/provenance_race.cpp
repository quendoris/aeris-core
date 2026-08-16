// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#include "aeris/storage/project.hpp"
#include "aeris/storage/provenance.hpp"

#include <atomic>
#include <filesystem>
#include <iostream>
#include <thread>

namespace {

void wait_for_start(std::atomic<int>& ready, std::atomic<bool>& go) {
    ready.fetch_add(1, std::memory_order_release);
    while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
}

aeris::storage::SourceSnapshotRecord record() {
    aeris::storage::SourceSnapshotRecord value;
    value.source_id = "source:race";
    value.adapter_id = "adapter.race.v1";
    value.capability_bits = 1U;
    value.temporal_class = 1U;
    value.provider = "Race Provider";
    value.dataset = "race-dataset";
    value.snapshot = "snapshot-1";
    value.dataset_version = "1";
    value.source_uri = "https://example.invalid/race/snapshot-1";
    value.license_id = "LicenseRef-Test";
    value.content_sha256 = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    value.retrieved_at_utc = "2026-08-16T19:50:00Z";
    value.resources = {{"payload", "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", 42U}};
    return value;
}

}  // namespace

int main() {
    using namespace aeris::storage;
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "aeris-provenance-race-test";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    if (ec) return 10;
    const std::filesystem::path path = root / "world.aeris";

    ProjectCreateOptions create;
    create.timestamp_utc = "2026-08-16T19:49:59Z";
    create.project_uuid = "99999999-8888-4777-8666-555555555555";
    auto first = ProjectStore::create(path, create);
    auto second = ProjectStore::open(path);
    if (!first.ok() || !second.ok()) return 11;

    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    SourceSnapshotMutationResult outcomes[2];
    SourceSnapshotRecord source = record();

    const auto source_worker = [&](const int index, ProjectStore& project, const char* timestamp) {
        wait_for_start(ready, go);
        outcomes[index] = store_source_snapshot(project, source, timestamp);
    };

    std::thread t1(source_worker, 0, std::ref(*first.store), "2026-08-16T19:50:01Z");
    std::thread t2(source_worker, 1, std::ref(*second.store), "2026-08-16T19:50:02Z");
    while (ready.load(std::memory_order_acquire) != 2) std::this_thread::yield();
    go.store(true, std::memory_order_release);
    t1.join();
    t2.join();

    if (!outcomes[0].ok() || !outcomes[1].ok()) {
        std::cerr << "concurrent idempotent source writes should both succeed\n";
        return 12;
    }
    const int inserted = static_cast<int>(outcomes[0].inserted) + static_cast<int>(outcomes[1].inserted);
    const int committed = static_cast<int>(outcomes[0].durably_committed) + static_cast<int>(outcomes[1].durably_committed);
    if (inserted != 1 || committed != 1) return 13;
    if (first.store->metadata().revision != 1U || second.store->metadata().revision != 1U) {
        std::cerr << "both project handles must refresh to the committed source revision\n";
        return 14;
    }
    auto listed = list_source_snapshots(*first.store);
    if (!listed.ok() || listed.records.size() != 1U) return 15;

    ready.store(0, std::memory_order_release);
    go.store(false, std::memory_order_release);
    Status metadata_status[2];
    const auto metadata_worker = [&](const int index, ProjectStore& project, const bool projection) {
        ProjectMetadataUpdate update;
        update.modified_utc = projection ? "2026-08-16T19:50:03Z" : "2026-08-16T19:50:04Z";
        if (projection) update.projection_id = "aeris.projection.concurrent.v1";
        else update.worldview_id = "concurrent-worldview";
        wait_for_start(ready, go);
        metadata_status[index] = project.update_metadata(update);
    };

    std::thread m1(metadata_worker, 0, std::ref(*first.store), true);
    std::thread m2(metadata_worker, 1, std::ref(*second.store), false);
    while (ready.load(std::memory_order_acquire) != 2) std::this_thread::yield();
    go.store(true, std::memory_order_release);
    m1.join();
    m2.join();
    if (!metadata_status[0].ok() || !metadata_status[1].ok()) return 16;

    if (!first.store->refresh_metadata().ok() || !second.store->refresh_metadata().ok()) return 17;
    const auto& final = first.store->metadata();
    if (final.revision != 3U || final.projection_id != "aeris.projection.concurrent.v1" ||
        final.worldview_id != "concurrent-worldview") {
        std::cerr << "serialized metadata updates must preserve independent fields and revisions\n";
        return 18;
    }
    if (second.store->metadata().revision != 3U || second.store->metadata().projection_id != final.projection_id ||
        second.store->metadata().worldview_id != final.worldview_id) return 19;

    first.store.reset();
    second.store.reset();
    std::filesystem::remove_all(root, ec);
    return ec ? 20 : 0;
}
