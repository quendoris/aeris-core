// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#include "aeris/storage/dataset.hpp"

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace {

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

aeris::storage::SourceDatasetRecord dataset_record() {
    using namespace aeris::storage;
    SourceDatasetRecord dataset{};
    dataset.provenance.source_id = "world.concurrent.atomic";
    dataset.provenance.adapter_id = "fixture.concurrent.v1";
    dataset.provenance.capability_bits = 1U;
    dataset.provenance.temporal_class = 1U;
    dataset.provenance.provider = "fixture-provider";
    dataset.provenance.dataset = "fixture-dataset";
    dataset.provenance.snapshot = "snapshot-1";
    dataset.provenance.dataset_version = "fixture-v1";
    dataset.provenance.source_uri = "fixture://dataset-race";
    dataset.provenance.license_id = "CC0-1.0";
    dataset.provenance.content_sha256 =
        "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";
    dataset.provenance.retrieved_at_utc = "2026-08-17T09:30:01Z";

    dataset.geometry.source_id = dataset.provenance.source_id;
    FeatureGeometryRecord feature{};
    feature.stable_id = "feature:race";
    feature.source_feature_id = "record:race";
    GeographicRingRecord ring{};
    ring.role = StoredRingRole::exterior;
    ring.interior_side = StoredInteriorSide::right;
    ring.vertices = {
        {0.0, 0.0},
        {0.2, 0.0},
        {0.2, 0.2},
        {0.0, 0.2},
    };
    ring.closing_longitude_rad = 0.0;
    feature.rings.push_back(std::move(ring));
    dataset.geometry.features.push_back(std::move(feature));
    return dataset;
}

}  // namespace

int main() {
    using namespace aeris::storage;

    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "aeris-dataset-race-contract";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    ec.clear();
    std::filesystem::create_directories(root, ec);
    expect(!ec, "dataset race root should be creatable");
    if (ec) return EXIT_FAILURE;

    const auto path = root / "world.aeris";
    ProjectCreateOptions options{};
    options.timestamp_utc = "2026-08-17T09:30:00Z";
    options.project_uuid = "90000000-0000-4000-8000-000000000009";
    auto created = ProjectStore::create(path, options);
    expect(created.ok(), "dataset race project should create");
    if (!created.ok()) return EXIT_FAILURE;
    created.store.reset();

    auto left = ProjectStore::open(path);
    auto right = ProjectStore::open(path);
    expect(left.ok() && right.ok(), "two independent dataset race handles should open");
    if (!left.ok() || !right.ok()) return EXIT_FAILURE;

    const SourceDatasetRecord dataset = dataset_record();
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    SourceDatasetMutationResult left_result{};
    SourceDatasetMutationResult right_result{};

    auto worker = [&](ProjectStore& project, SourceDatasetMutationResult& result) {
        ready.fetch_add(1, std::memory_order_release);
        while (!go.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        result = store_source_dataset(project, dataset, "2026-08-17T09:30:02Z");
    };

    std::thread left_thread(worker, std::ref(*left.store), std::ref(left_result));
    std::thread right_thread(worker, std::ref(*right.store), std::ref(right_result));
    while (ready.load(std::memory_order_acquire) != 2) {
        std::this_thread::yield();
    }
    go.store(true, std::memory_order_release);
    left_thread.join();
    right_thread.join();

    expect(left_result.ok() && right_result.ok(),
           "concurrent identical dataset ingestions should both succeed semantically");
    const int inserted_count =
        (left_result.inserted ? 1 : 0) + (right_result.inserted ? 1 : 0);
    const int committed_count =
        (left_result.durably_committed ? 1 : 0) +
        (right_result.durably_committed ? 1 : 0);
    expect(inserted_count == 1,
           "exactly one concurrent dataset writer should report insertion");
    expect(committed_count == 1,
           "exactly one concurrent dataset writer should report durable commit");

    auto reopened = ProjectStore::open(path);
    expect(reopened.ok(), "dataset race project should reopen");
    if (reopened.ok()) {
        expect(reopened.store->metadata().revision == 1U,
               "concurrent identical dataset ingestion must produce one project revision");
        const auto sources = list_source_snapshots(*reopened.store);
        const auto geometry = list_source_geometry_index(*reopened.store, dataset.provenance.source_id);
        expect(sources.ok() && sources.records.size() == 1U,
               "concurrent dataset ingestion must persist one provenance record");
        expect(geometry.ok() && geometry.features.size() == 1U,
               "concurrent dataset ingestion must persist one complete geometry set");
        expect(reopened.store->verify_integrity().ok(),
               "concurrent dataset result should pass deep project audit");
    }

    std::filesystem::remove_all(root, ec);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
