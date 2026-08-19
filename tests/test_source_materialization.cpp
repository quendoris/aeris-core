// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/storage/dataset.hpp"
#include "aeris/storage/geometry.hpp"
#include "aeris/storage/layer.hpp"
#include "aeris/storage/provenance.hpp"
#include "aeris/storage/resource.hpp"

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

class Fixture final {
public:
    Fixture() {
        root_ = std::filesystem::temp_directory_path() / "aeris-source-materialization-v0";
        std::error_code ignored{};
        std::filesystem::remove_all(root_, ignored);
        std::filesystem::create_directories(root_);

        aeris::storage::ProjectCreateOptions options{};
        options.timestamp_utc = "2026-08-19T08:00:00Z";
        options.project_uuid = "12345678-90ab-4cde-8fab-1234567890ab";
        auto created = aeris::storage::ProjectStore::create(path(), options);
        if (created.ok()) project_ = std::move(created.store);
    }

    ~Fixture() {
        project_.reset();
        std::error_code ignored{};
        std::filesystem::remove_all(root_, ignored);
    }

    [[nodiscard]] aeris::storage::ProjectStore* project() noexcept { return project_.get(); }
    [[nodiscard]] std::filesystem::path path() const { return root_ / "world.aeris"; }

private:
    std::filesystem::path root_;
    std::unique_ptr<aeris::storage::ProjectStore> project_;
};

[[nodiscard]] aeris::storage::SourceSnapshotRecord referenced_source(
    const std::string& source_id) {
    aeris::storage::SourceSnapshotRecord source{};
    source.source_id = source_id;
    source.adapter_id = "fixture.vector.v1";
    source.capability_bits = 1U;
    source.temporal_class = 0U;
    source.provider = "Fixture";
    source.dataset = "world";
    source.snapshot = "v1";
    source.dataset_version = "1";
    source.source_uri = "https://example.invalid/world";
    source.license_id = "CC0-1.0";
    source.content_sha256 = std::string(64U, 'a');
    source.retrieved_at_utc.clear();
    source.worldview = "fixture";
    source.materialization_state = aeris::storage::SourceMaterializationState::referenced;

    aeris::storage::SourceResourceRecord resource{};
    resource.logical_name = "geometry";
    resource.sha256 = std::string(64U, 'b');
    resource.size_bytes = 123U;
    resource.relative_path = "data/world.bin";
    resource.retrieval_uri = "https://example.invalid/data/world.bin";
    source.resources.push_back(std::move(resource));
    return source;
}

[[nodiscard]] aeris::storage::SourceDatasetRecord materialized_dataset(
    const aeris::storage::SourceSnapshotRecord& reference) {
    aeris::storage::SourceDatasetRecord dataset{};
    dataset.source = reference;
    dataset.source.materialization_state =
        aeris::storage::SourceMaterializationState::materialized;
    dataset.source.retrieved_at_utc = "2026-08-19T08:00:03Z";

    // A mirror locator is allowed to differ from the stored reference recipe:
    // content hashes define immutable source identity, not the transport URL.
    dataset.source.resources.front().retrieval_uri =
        "https://mirror.invalid/world.bin";

    dataset.geometry.source_id = reference.source_id;
    aeris::storage::FeatureGeometryRecord feature{};
    feature.stable_id = "feature-1";
    feature.source_feature_id = "source-feature-1";

    aeris::storage::GeographicRingRecord ring{};
    ring.role = aeris::storage::StoredRingRole::exterior;
    ring.interior_side = aeris::storage::StoredInteriorSide::unspecified;
    ring.longitude_winding = 0;
    ring.closing_longitude_rad = 0.0;
    ring.vertices = {
        {0.0, 0.0},
        {0.1, 0.0},
        {0.0, 0.1},
    };
    feature.rings.push_back(std::move(ring));
    dataset.geometry.features.push_back(std::move(feature));
    return dataset;
}

[[nodiscard]] bool raw_exec_fails(
    const std::filesystem::path& path,
    const char* sql) {
    sqlite3* db = nullptr;
    const std::string native = path.string();
    if (sqlite3_open_v2(native.c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK ||
        db == nullptr) {
        if (db != nullptr) sqlite3_close(db);
        return false;
    }
    char* error = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &error);
    if (error != nullptr) sqlite3_free(error);
    sqlite3_close(db);
    return rc != SQLITE_OK;
}

void test_source_materialization() {
    using namespace aeris::storage;

    Fixture fixture{};
    ProjectStore* project = fixture.project();
    expect_true("materialization project creates", project != nullptr);
    if (project == nullptr) return;
    expect_true("generation nine constants active",
                project->metadata().format_major == kDraftFormatMajor &&
                project->metadata().format_minor == kDraftFormatMinor &&
                kProjectSchemaGeneration == 9);

    SourceSnapshotRecord reference = referenced_source("world.online");
    expect_true("portable reference is fetchable", source_reference_is_fetchable(reference));

    const auto stored_reference = store_source_snapshot(
        *project, reference, "2026-08-19T08:00:01Z");
    expect_true("referenced source commits once",
                stored_reference.ok() && stored_reference.inserted &&
                stored_reference.durably_committed);
    expect_true("reference is revision one", project->metadata().revision == 1U);

    const auto listed_reference = list_source_snapshots(*project);
    expect_true("referenced source lists", listed_reference.ok() && listed_reference.records.size() == 1U);
    if (listed_reference.ok() && listed_reference.records.size() == 1U) {
        const SourceSnapshotRecord& stored = listed_reference.records.front();
        expect_true("stored source remains referenced",
                    stored.materialization_state == SourceMaterializationState::referenced);
        expect_true("referenced source has no retrieval timestamp", stored.retrieved_at_utc.empty());
        expect_true("stored acquisition path preserves",
                    stored.resources.size() == 1U &&
                    stored.resources.front().relative_path == "data/world.bin");
        expect_true("stored acquisition URI preserves",
                    stored.resources.size() == 1U &&
                    stored.resources.front().retrieval_uri ==
                        "https://example.invalid/data/world.bin");
    }

    const auto missing_geometry = list_source_geometry_index(*project, reference.source_id);
    expect_true("referenced source has no canonical geometry",
                !missing_geometry.ok() &&
                missing_geometry.status.error == StorageError::record_not_found);

    LayerCreateRequest layer{};
    layer.layer_id = "online-countries";
    layer.role_id = std::string(kLayerRolePoliticalCountryFillV1);
    layer.name = "Online countries";
    layer.sources.push_back({"geometry", reference.source_id});
    const auto layer_write = append_layer(
        *project, layer, "2026-08-19T08:00:02Z");
    expect_true("unfrozen project may bind referenced source",
                layer_write.ok() && layer_write.changed && layer_write.durably_committed);
    expect_true("layer binding is revision two", project->metadata().revision == 2U);

    const auto premature_freeze = freeze_project(*project, "2026-08-19T08:00:03Z");
    expect_true("freeze rejects layer-bound referenced source", !premature_freeze.ok());
    expect_true("failed freeze preserves revision two", project->metadata().revision == 2U);
    expect_true("failed freeze preserves thawed state", !project->metadata().frozen);

    expect_true("raw SQLite freeze is blocked by trigger",
                raw_exec_fails(fixture.path(), "UPDATE aeris_meta SET frozen=1 WHERE id=1;"));
    expect_true("triggered raw freeze leaves project valid", project->verify_integrity().ok());

    SourceDatasetRecord dataset = materialized_dataset(reference);
    const auto downloaded = store_source_dataset(
        *project, dataset, "2026-08-19T08:00:04Z");
    expect_true("referenced source materializes atomically",
                downloaded.ok() && downloaded.inserted && downloaded.durably_committed);
    expect_true("download is one new revision", project->metadata().revision == 3U);

    const auto listed_materialized = list_source_snapshots(*project);
    expect_true("materialized source lists", listed_materialized.ok() && listed_materialized.records.size() == 1U);
    if (listed_materialized.ok() && listed_materialized.records.size() == 1U) {
        const SourceSnapshotRecord& stored = listed_materialized.records.front();
        expect_true("source state becomes materialized",
                    stored.materialization_state == SourceMaterializationState::materialized);
        expect_true("verified retrieval timestamp commits",
                    stored.retrieved_at_utc == "2026-08-19T08:00:03Z");
        expect_true("materialization does not rewrite stored locator",
                    stored.resources.front().retrieval_uri ==
                        "https://example.invalid/data/world.bin");
    }

    const auto geometry = list_source_geometry_index(*project, reference.source_id);
    expect_true("materialized source owns canonical geometry",
                geometry.ok() && geometry.features.size() == 1U);

    const auto retry = store_source_dataset(
        *project, dataset, "2026-08-19T08:00:05Z");
    expect_true("exact materialization retry is idempotent",
                retry.ok() && !retry.inserted && !retry.durably_committed);
    expect_true("retry keeps revision three", project->metadata().revision == 3U);

    const auto frozen = freeze_project(*project, "2026-08-19T08:00:06Z");
    expect_true("materialized bound source can freeze",
                frozen.ok() && frozen.changed && frozen.durably_committed);
    expect_true("freeze is revision four", project->metadata().revision == 4U);
    expect_true("project reports frozen", project->metadata().frozen);

    expect_true("raw source demotion is blocked permanently",
                raw_exec_fails(
                    fixture.path(),
                    "UPDATE aeris_source SET materialization_state=0 WHERE source_id='world.online';"));

    SourceSnapshotRecord unused = referenced_source("world.unused-online");
    unused.content_sha256 = std::string(64U, 'c');
    unused.resources.front().sha256 = std::string(64U, 'd');
    const auto unused_write = store_source_snapshot(
        *project, unused, "2026-08-19T08:00:07Z");
    expect_true("frozen project may catalog unbound reference",
                unused_write.ok() && unused_write.inserted && unused_write.durably_committed);
    expect_true("unbound catalog insert advances revision five",
                project->metadata().revision == 5U);
    expect_true("catalog insert does not thaw project", project->metadata().frozen);

    LayerCreateRequest forbidden{};
    forbidden.layer_id = "forbidden-online-layer";
    forbidden.role_id = std::string(kLayerRolePhysicalLandFillV1);
    forbidden.name = "Forbidden online layer";
    forbidden.sources.push_back({"geometry", unused.source_id});
    const auto forbidden_bind = append_layer(
        *project, forbidden, "2026-08-19T08:00:08Z");
    expect_true("frozen project cannot bind referenced source", !forbidden_bind.ok());
    expect_true("failed layer bind preserves revision five", project->metadata().revision == 5U);

    const auto layers = list_project_layers(*project);
    expect_true("failed layer bind rolls back completely",
                layers.ok() && layers.records.size() == 1U &&
                layers.records.front().layer_id == "online-countries");
    expect_true("final materialization project passes deep integrity",
                project->verify_integrity().ok());
}

void test_invalid_acquisition_recipe() {
    using namespace aeris::storage;

    Fixture fixture{};
    ProjectStore* project = fixture.project();
    if (project == nullptr) {
        expect_true("invalid recipe fixture creates", false);
        return;
    }

    SourceSnapshotRecord file_uri = referenced_source("bad.file-uri");
    file_uri.resources.front().retrieval_uri = "file:///tmp/world.bin";
    const auto rejected_file_uri = store_source_snapshot(
        *project, file_uri, "2026-08-19T08:01:00Z");
    expect_true("machine-local file URI is rejected", !rejected_file_uri.ok());

    SourceSnapshotRecord traversal = referenced_source("bad.traversal");
    traversal.resources.front().relative_path = "../world.bin";
    const auto rejected_traversal = store_source_snapshot(
        *project, traversal, "2026-08-19T08:01:01Z");
    expect_true("relative path traversal is rejected", !rejected_traversal.ok());

    SourceSnapshotRecord half_recipe = referenced_source("bad.half-recipe");
    half_recipe.resources.front().retrieval_uri.clear();
    const auto rejected_half = store_source_snapshot(
        *project, half_recipe, "2026-08-19T08:01:02Z");
    expect_true("half acquisition recipe is rejected", !rejected_half.ok());
    expect_true("invalid recipes never advance revision", project->metadata().revision == 0U);
}

}  // namespace

int main() {
    test_source_materialization();
    test_invalid_acquisition_recipe();
    if (failures != 0) {
        std::cerr << failures << " source materialization assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "project_storage_source_materialization: PASS\n";
    return EXIT_SUCCESS;
}
