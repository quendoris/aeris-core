// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#include "aeris/storage/dataset.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void expect_error(
    const aeris::storage::Status& status,
    const aeris::storage::StorageError expected,
    const std::string_view message) {
    if (status.error != expected) {
        ++failures;
        std::cerr << "FAIL: " << message
                  << " expected=" << static_cast<int>(expected)
                  << " actual=" << static_cast<int>(status.error)
                  << " diagnostic=" << status.diagnostic << '\n';
    }
}

aeris::storage::SourceDatasetRecord dataset_record(const std::string& source_id) {
    using namespace aeris::storage;

    SourceDatasetRecord dataset{};
    dataset.provenance.source_id = source_id;
    dataset.provenance.adapter_id = "fixture.dataset.adapter.v1";
    dataset.provenance.capability_bits = 1U;
    dataset.provenance.temporal_class = 1U;
    dataset.provenance.provider = "fixture-provider";
    dataset.provenance.dataset = "fixture-dataset";
    dataset.provenance.snapshot = "snapshot-1";
    dataset.provenance.dataset_version = "fixture-v1";
    dataset.provenance.source_uri = "fixture://atomic-dataset";
    dataset.provenance.license_id = "CC0-1.0";
    dataset.provenance.content_sha256 =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    dataset.provenance.retrieved_at_utc = "2026-08-17T09:00:01Z";
    dataset.provenance.resources = {
        {"geometry", "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", 64U},
        {"metadata", "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc", 16U},
    };

    dataset.geometry.source_id = source_id;
    FeatureGeometryRecord feature{};
    feature.stable_id = "feature:1";
    feature.source_feature_id = "record:1";
    GeographicRingRecord ring{};
    ring.role = StoredRingRole::exterior;
    ring.interior_side = StoredInteriorSide::right;
    ring.longitude_winding = 0;
    ring.closing_longitude_rad = 0.0;
    ring.vertices = {
        {0.0, 0.0},
        {0.2, 0.0},
        {0.2, 0.2},
        {0.0, 0.2},
    };
    feature.rings.push_back(std::move(ring));
    dataset.geometry.features.push_back(std::move(feature));
    return dataset;
}

std::unique_ptr<aeris::storage::ProjectStore> create_project(
    const std::filesystem::path& path,
    const std::string& uuid) {
    aeris::storage::ProjectCreateOptions options{};
    options.timestamp_utc = "2026-08-17T09:00:00Z";
    options.project_uuid = uuid;
    auto created = aeris::storage::ProjectStore::create(path, options);
    if (!created.ok()) {
        std::cerr << "project create failed: " << created.status.diagnostic << '\n';
        return {};
    }
    return std::move(created.store);
}

void test_atomic_insert_retry_and_reopen(const std::filesystem::path& root) {
    using namespace aeris::storage;
    const auto path = root / "atomic.aeris";
    auto project = create_project(path, "40000000-0000-4000-8000-000000000004");
    expect(project != nullptr, "atomic dataset fixture project should create");
    if (!project) return;

    const auto dataset = dataset_record("world.land.atomic");
    const auto first = store_source_dataset(*project, dataset, "2026-08-17T09:00:02Z");
    expect(first.ok() && first.inserted && first.durably_committed,
           "new source dataset should commit atomically");
    expect(project->metadata().revision == 1U,
           "provenance plus geometry must advance exactly one revision");

    const auto sources = list_source_snapshots(*project);
    const auto geometry = list_source_geometry_index(*project, dataset.provenance.source_id);
    expect(sources.ok() && sources.records.size() == 1U,
           "atomic dataset commit should expose provenance");
    expect(geometry.ok() && geometry.features.size() == 1U,
           "atomic dataset commit should expose geometry");

    const auto retry = store_source_dataset(*project, dataset, "2026-08-17T09:00:03Z");
    expect(retry.ok() && !retry.inserted && !retry.durably_committed,
           "exact atomic dataset retry should be idempotent");
    expect(project->metadata().revision == 1U,
           "idempotent dataset retry must not advance revision");

    project.reset();
    auto reopened = ProjectStore::open(path);
    expect(reopened.ok(), "atomic dataset project should reopen after close");
    if (!reopened.ok()) return;
    expect(reopened.store->metadata().revision == 1U,
           "single atomic dataset revision should survive reopen");
    expect(list_source_snapshots(*reopened.store).records.size() == 1U,
           "reopened project should retain dataset provenance");
    expect(list_source_geometry_index(*reopened.store, dataset.provenance.source_id).features.size() == 1U,
           "reopened project should retain dataset geometry");
    expect(reopened.store->verify_integrity().ok(),
           "atomic dataset project should pass deep integrity audit");
}

void test_invalid_geometry_leaves_no_provenance(const std::filesystem::path& root) {
    using namespace aeris::storage;
    const auto path = root / "rollback.aeris";
    auto project = create_project(path, "50000000-0000-4000-8000-000000000005");
    if (!project) {
        ++failures;
        return;
    }

    auto dataset = dataset_record("world.invalid.atomic");
    dataset.geometry.features.front().rings.front().longitude_winding = 1;
    // closing_longitude remains zero, so the geometry contract is invalid.
    const auto result = store_source_dataset(*project, dataset, "2026-08-17T09:01:00Z");
    expect_error(result.status, StorageError::invalid_argument,
                 "invalid geometry must reject the combined dataset before mutation");
    expect(project->metadata().revision == 0U,
           "invalid combined dataset must leave revision zero");
    const auto sources = list_source_snapshots(*project);
    expect(sources.ok() && sources.records.empty(),
           "invalid combined dataset must not leak provenance into project");
    const auto geometry = list_source_geometry_index(*project, dataset.provenance.source_id);
    expect_error(geometry.status, StorageError::record_not_found,
                 "invalid combined dataset must not create geometry marker");
}

void test_partial_low_level_state_fails_closed(const std::filesystem::path& root) {
    using namespace aeris::storage;
    const auto path = root / "partial.aeris";
    auto project = create_project(path, "60000000-0000-4000-8000-000000000006");
    if (!project) {
        ++failures;
        return;
    }

    const auto dataset = dataset_record("world.partial.atomic");
    const auto provenance_only = store_source_snapshot(
        *project, dataset.provenance, "2026-08-17T09:02:00Z");
    expect(provenance_only.ok() && provenance_only.inserted,
           "low-level provenance-only fixture should commit");
    expect(project->metadata().revision == 1U,
           "low-level provenance fixture should own its revision");

    const auto combined = store_source_dataset(
        *project, dataset, "2026-08-17T09:02:01Z");
    expect_error(combined.status, StorageError::record_exists,
                 "combined ingest must reject pre-existing partial low-level state");
    expect(project->metadata().revision == 1U,
           "rejected partial state must not gain a second revision");
    const auto geometry = list_source_geometry_index(*project, dataset.provenance.source_id);
    expect_error(geometry.status, StorageError::record_not_found,
                 "combined ingest must not silently complete provenance-only history");
}

void test_conflict_and_empty_dataset(const std::filesystem::path& root) {
    using namespace aeris::storage;
    const auto path = root / "conflict-empty.aeris";
    auto project = create_project(path, "70000000-0000-4000-8000-000000000007");
    if (!project) {
        ++failures;
        return;
    }

    auto dataset = dataset_record("world.dataset.conflict");
    const auto first = store_source_dataset(*project, dataset, "2026-08-17T09:03:00Z");
    expect(first.ok(), "conflict fixture initial dataset should commit");
    dataset.geometry.features.front().rings.front().vertices[1].latitude_rad = 0.01;
    const auto conflict = store_source_dataset(*project, dataset, "2026-08-17T09:03:01Z");
    expect_error(conflict.status, StorageError::record_exists,
                 "different immutable geometry under existing dataset ID must conflict");
    expect(project->metadata().revision == 1U,
           "dataset conflict must not advance revision");

    auto empty = dataset_record("world.dataset.empty");
    empty.provenance.content_sha256 =
        "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";
    empty.geometry.features.clear();
    const auto empty_result = store_source_dataset(*project, empty, "2026-08-17T09:03:02Z");
    expect(empty_result.ok() && empty_result.inserted && empty_result.durably_committed,
           "combined ingest should atomically represent verified empty geometry");
    expect(project->metadata().revision == 2U,
           "empty dataset should still be one semantic revision");
    const auto empty_index = list_source_geometry_index(*project, empty.provenance.source_id);
    expect(empty_index.ok() && empty_index.features.empty(),
           "empty combined dataset must persist explicit empty geometry marker");
}

}  // namespace

int main() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "aeris-dataset-storage-contract";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    ec.clear();
    std::filesystem::create_directories(root, ec);
    expect(!ec, "dataset storage test root should be creatable");
    if (ec) return EXIT_FAILURE;

    test_atomic_insert_retry_and_reopen(root);
    test_invalid_geometry_leaves_no_provenance(root);
    test_partial_low_level_state_fails_closed(root);
    test_conflict_and_empty_dataset(root);

    std::filesystem::remove_all(root, ec);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
