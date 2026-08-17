// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#include "aeris/storage/project.hpp"
#include "aeris/storage/projection.hpp"
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

    const auto projection_worker = [&] {
        ProjectProjectionRecord projection{};
        projection.model_id = "aeris.projection.concurrent.v1";
        projection.central_meridian_rad = 0.25;
        projection.cut_model_id = "aeris.cut.concurrent.v1";
        wait_for_start(ready, go);
        metadata_status[0] = set_project_projection(
            *first.store, projection, "2026-08-16T19:50:03Z").status;
    };
    const auto worldview_worker = [&] {
        ProjectMetadataUpdate update;
        update.modified_utc = "2026-08-16T19:50:04Z";
        update.worldview_id = "concurrent-worldview";
        wait_for_start(ready, go);
        metadata_status[1] = second.store->update_metadata(update);
    };

    std::thread m1(projection_worker);
    std::thread m2(worldview_worker);
    while (ready.load(std::memory_order_acquire) != 2) std::this_thread::yield();
    go.store(true, std::memory_order_release);
    m1.join();
    m2.join();
    if (!metadata_status[0].ok() || !metadata_status[1].ok()) {
        std::cerr << "structured projection and worldview mutations should serialize successfully\n";
        return 16;
    }

    if (!first.store->refresh_metadata().ok() || !second.store->refresh_metadata().ok()) return 17;
    const auto& final = first.store->metadata();
    if (final.revision != 3U || final.projection_id != "aeris.projection.concurrent.v1" ||
        final.worldview_id != "concurrent-worldview") {
        std::cerr << "serialized structured projection/worldview updates must preserve independent state and revisions\n";
        return 18;
    }
    if (second.store->metadata().revision != 3U || second.store->metadata().projection_id != final.projection_id ||
        second.store->metadata().worldview_id != final.worldview_id) return 19;

    const auto projection = load_project_projection(*first.store);
    if (!projection.ok() || projection.record.model_id != "aeris.projection.concurrent.v1" ||
        projection.record.central_meridian_rad != 0.25 ||
        projection.record.cut_model_id != "aeris.cut.concurrent.v1") {
        std::cerr << "structured projection must survive concurrent worldview mutation\n";
        return 20;
    }

    first.store.reset();
    second.store.reset();
    std::filesystem::remove_all(root, ec);
    return ec ? 21 : 0;
}
