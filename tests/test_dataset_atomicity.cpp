// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/storage/dataset.hpp"
#include "aeris/storage/geometry.hpp"
#include "aeris/storage/provenance.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

#include <sqlite3.h>

namespace {

int failures = 0;

void expect_true(const std::string_view name, const bool condition) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL " << name << '\n';
    }
}

class TempProject final {
public:
    TempProject() {
        const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        root_ = std::filesystem::temp_directory_path() /
                ("aeris-dataset-atomicity-" + std::to_string(stamp));
        std::filesystem::create_directories(root_);

        aeris::storage::ProjectCreateOptions options{};
        options.timestamp_utc = "2026-08-17T08:20:00Z";
        auto created = aeris::storage::ProjectStore::create(root_ / "world.aeris", options);
        if (created.ok()) project_ = std::move(created.store);
    }

    ~TempProject() {
        project_.reset();
        std::error_code ignored{};
        std::filesystem::remove_all(root_, ignored);
    }

    [[nodiscard]] aeris::storage::ProjectStore* get() noexcept { return project_.get(); }

private:
    std::filesystem::path root_;
    std::unique_ptr<aeris::storage::ProjectStore> project_;
};

[[nodiscard]] aeris::storage::SourceDatasetRecord make_dataset() {
    aeris::storage::SourceDatasetRecord dataset{};
    dataset.source.source_id = "world.land.primary";
    dataset.source.adapter_id = "fixture.adapter.v1";
    dataset.source.capability_bits = 1U;
    dataset.source.temporal_class = 1U;
    dataset.source.provider = "fixture-provider";
    dataset.source.dataset = "fixture-dataset";
    dataset.source.snapshot = "snapshot-1";
    dataset.source.dataset_version = "v1";
    dataset.source.source_uri = "fixture://dataset";
    dataset.source.license_id = "CC0-1.0";
    dataset.source.content_sha256 = std::string(64U, 'a');
    dataset.source.retrieved_at_utc = "2026-08-17T08:19:00Z";
    dataset.source.worldview = "";
    dataset.source.resources.push_back({"land", std::string(64U, 'b'), 123U});

    dataset.geometry.source_id = dataset.source.source_id;
    aeris::storage::FeatureGeometryRecord feature{};
    feature.stable_id = "feature-1";
    feature.source_feature_id = "source-feature-1";

    aeris::storage::GeographicRingRecord ring{};
    ring.role = aeris::storage::StoredRingRole::exterior;
    ring.interior_side = aeris::storage::StoredInteriorSide::left;
    ring.longitude_winding = 0;
    ring.closing_longitude_rad = -0.2;
    ring.vertices = {{-0.2, -0.1}, {0.2, -0.1}, {0.2, 0.1}, {-0.2, 0.1}};
    feature.rings.push_back(std::move(ring));
    dataset.geometry.features.push_back(std::move(feature));
    return dataset;
}

void test_full_dataset_is_one_revision_and_idempotent() {
    TempProject fixture{};
    auto* project = fixture.get();
    expect_true("dataset project creates", project != nullptr);
    if (project == nullptr) return;

    auto dataset = make_dataset();
    const auto first = aeris::storage::store_source_dataset(
        *project, dataset, "2026-08-17T08:21:00Z");
    expect_true("dataset first insert succeeds", first.ok());
    expect_true("dataset first insert reports inserted", first.inserted);
    expect_true("dataset first insert reports durable", first.durably_committed);
    expect_true("dataset provenance plus geometry is one revision", project->metadata().revision == 1U);

    const auto sources = aeris::storage::list_source_snapshots(*project);
    expect_true("dataset provenance lists", sources.ok() && sources.records.size() == 1U);
    const auto geometry = aeris::storage::list_source_geometry_index(
        *project, dataset.source.source_id);
    expect_true("dataset geometry marker lists", geometry.ok() && geometry.features.size() == 1U);

    const auto retry = aeris::storage::store_source_dataset(
        *project, dataset, "2026-08-17T08:22:00Z");
    expect_true("dataset exact retry succeeds", retry.ok());
    expect_true("dataset exact retry does not insert", !retry.inserted);
    expect_true("dataset exact retry does not commit", !retry.durably_committed);
    expect_true("dataset exact retry keeps revision", project->metadata().revision == 1U);

    dataset.geometry.features.front().rings.front().vertices[1].latitude_rad = -0.05;
    const auto conflict = aeris::storage::store_source_dataset(
        *project, dataset, "2026-08-17T08:23:00Z");
    expect_true(
        "dataset geometry conflict rejected",
        conflict.status.error == aeris::storage::StorageError::record_exists);
    expect_true("dataset geometry conflict keeps revision", project->metadata().revision == 1U);
}

void test_geometry_failure_rolls_back_provenance() {
    TempProject fixture{};
    auto* project = fixture.get();
    expect_true("rollback project creates", project != nullptr);
    if (project == nullptr) return;

    sqlite3* raw = nullptr;
    const std::string path = project->path().string();
    const int open_rc = sqlite3_open_v2(
        path.c_str(), &raw, SQLITE_OPEN_READWRITE, nullptr);
    expect_true("rollback trigger database opens", open_rc == SQLITE_OK && raw != nullptr);
    if (open_rc != SQLITE_OK || raw == nullptr) {
        if (raw != nullptr) sqlite3_close(raw);
        return;
    }

    const char* trigger_sql =
        "CREATE TRIGGER aeris_force_geometry_abort "
        "BEFORE INSERT ON aeris_source_geometry "
        "BEGIN SELECT RAISE(ABORT,'forced geometry abort'); END;";
    char* error = nullptr;
    const int trigger_rc = sqlite3_exec(raw, trigger_sql, nullptr, nullptr, &error);
    if (error != nullptr) sqlite3_free(error);
    expect_true("rollback trigger installs", trigger_rc == SQLITE_OK);
    sqlite3_close(raw);
    if (trigger_rc != SQLITE_OK) return;

    const auto dataset = make_dataset();
    const auto failed = aeris::storage::store_source_dataset(
        *project, dataset, "2026-08-17T08:24:00Z");
    expect_true("forced geometry failure is reported", !failed.ok());
    expect_true("forced geometry failure is not durable", !failed.durably_committed);
    expect_true("forced geometry failure leaves revision zero", project->metadata().revision == 0U);

    const auto sources = aeris::storage::list_source_snapshots(*project);
    expect_true(
        "forced geometry failure rolls provenance back",
        sources.ok() && sources.records.empty());
    const auto geometry = aeris::storage::list_source_geometry_index(
        *project, dataset.source.source_id);
    expect_true(
        "forced geometry failure leaves no geometry marker",
        geometry.status.error == aeris::storage::StorageError::record_not_found);
}

void test_identical_provenance_can_gain_explicit_geometry() {
    TempProject fixture{};
    auto* project = fixture.get();
    expect_true("attach project creates", project != nullptr);
    if (project == nullptr) return;

    const auto dataset = make_dataset();
    const auto provenance = aeris::storage::store_source_snapshot(
        *project, dataset.source, "2026-08-17T08:25:00Z");
    expect_true("lower-level provenance insert succeeds", provenance.ok());
    expect_true("lower-level provenance is first revision", project->metadata().revision == 1U);

    const auto attached = aeris::storage::store_source_dataset(
        *project, dataset, "2026-08-17T08:26:00Z");
    expect_true("identical provenance accepts geometry attachment", attached.ok());
    expect_true("geometry attachment commits", attached.durably_committed);
    expect_true("geometry attachment is one additional revision", project->metadata().revision == 2U);

    const auto geometry = aeris::storage::list_source_geometry_index(
        *project, dataset.source.source_id);
    expect_true("attached geometry lists", geometry.ok() && geometry.features.size() == 1U);
}

}  // namespace

int main() {
    test_full_dataset_is_one_revision_and_idempotent();
    test_geometry_failure_rolls_back_provenance();
    test_identical_provenance_can_gain_explicit_geometry();

    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "project_storage_dataset_atomicity: PASS\n";
    return EXIT_SUCCESS;
}
