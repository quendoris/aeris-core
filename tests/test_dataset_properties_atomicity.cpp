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

void expect_true(const std::string_view name, const bool condition) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL " << name << '\n';
    }
}

void expect_error(
    const std::string_view name,
    const aeris::storage::Status& status,
    const aeris::storage::StorageError expected) {
    if (status.error != expected) {
        ++failures;
        std::cerr << "FAIL " << name
                  << " expected=" << static_cast<int>(expected)
                  << " actual=" << static_cast<int>(status.error)
                  << " diagnostic=" << status.diagnostic << '\n';
    }
}

aeris::storage::GeographicRingRecord ring(double offset) {
    using namespace aeris::storage;
    GeographicRingRecord value{};
    value.role = StoredRingRole::exterior;
    value.interior_side = StoredInteriorSide::right;
    value.vertices = {
        {offset + 0.00, 0.00},
        {offset + 0.10, 0.00},
        {offset + 0.10, 0.10},
        {offset + 0.00, 0.10},
    };
    value.closing_longitude_rad = offset;
    return value;
}

aeris::storage::SourceDatasetRecord dataset(
    std::string source_id,
    char hash_char,
    bool with_properties) {
    using namespace aeris::storage;
    SourceDatasetRecord record{};
    record.source.source_id = source_id;
    record.source.adapter_id = "fixture.dataset.v1";
    record.source.capability_bits = 2U;
    record.source.temporal_class = 1U;
    record.source.provider = "Fixture Provider";
    record.source.dataset = "admin0";
    record.source.snapshot = "snapshot-1";
    record.source.dataset_version = "v1";
    record.source.source_uri = "fixture://" + source_id;
    record.source.license_id = "CC0-1.0";
    record.source.content_sha256 = std::string(64U, hash_char);
    record.source.retrieved_at_utc = "2026-08-17T11:40:00Z";

    record.geometry.source_id = source_id;
    FeatureGeometryRecord b{};
    b.stable_id = "feature:b";
    b.source_feature_id = "record:2";
    b.rings.push_back(ring(0.4));
    FeatureGeometryRecord a{};
    a.stable_id = "feature:a";
    a.source_feature_id = "record:1";
    a.rings.push_back(ring(0.0));
    record.geometry.features.push_back(std::move(b));
    record.geometry.features.push_back(std::move(a));

    if (with_properties) {
        record.feature_properties.emplace();
        record.feature_properties->source_id = source_id;
        FeaturePropertiesRecord b_properties{};
        b_properties.stable_id = "feature:b";
        FeaturePropertiesRecord a_properties{};
        a_properties.stable_id = "feature:a";
        a_properties.properties.push_back({"iso_a2", std::string("AA")});
        a_properties.properties.push_back({"name", std::string("Alpha")});
        record.feature_properties->features.push_back(std::move(b_properties));
        record.feature_properties->features.push_back(std::move(a_properties));
    }
    return record;
}

bool source_exists(
    const aeris::storage::ProjectStore& project,
    const std::string_view source_id) {
    const auto sources = aeris::storage::list_source_snapshots(project);
    if (!sources.ok()) return false;
    for (const auto& source : sources.records) {
        if (source.source_id == source_id) return true;
    }
    return false;
}

}  // namespace

int main() {
    using namespace aeris::storage;

    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "aeris-dataset-properties-atomicity";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    expect_true("dataset property test directory creates", !ec);

    ProjectCreateOptions options{};
    options.timestamp_utc = "2026-08-17T11:39:59Z";
    options.project_uuid = "32345678-1234-4abc-8def-1234567890ab";
    auto project = ProjectStore::create(root / "world.aeris", options);
    expect_true("dataset property project creates", project.ok());
    if (!project.ok()) return EXIT_FAILURE;

    SourceDatasetRecord complete = dataset("dataset.complete", 'a', true);
    const auto first = store_source_dataset(
        *project.store, complete, "2026-08-17T11:40:01Z");
    expect_true("complete source dataset commits atomically",
                first.ok() && first.inserted && first.durably_committed);
    expect_true("provenance geometry properties share one revision",
                project.store->metadata().revision == 1U);
    expect_true("complete dataset provenance visible", source_exists(*project.store, "dataset.complete"));
    const auto complete_geometry = list_source_geometry_index(*project.store, "dataset.complete");
    expect_true("complete dataset geometry visible",
                complete_geometry.ok() && complete_geometry.features.size() == 2U);
    const auto complete_properties = list_source_feature_properties_index(
        *project.store, "dataset.complete");
    expect_true("complete dataset property marker visible",
                complete_properties.ok() && complete_properties.features.size() == 2U);

    const auto retry = store_source_dataset(
        *project.store, complete, "2026-08-17T11:40:02Z");
    expect_true("exact complete dataset retry is idempotent",
                retry.ok() && !retry.inserted && !retry.durably_committed);
    expect_true("complete dataset retry keeps revision one",
                project.store->metadata().revision == 1U);

    SourceDatasetRecord invalid = dataset("dataset.invalid", 'b', true);
    invalid.feature_properties->features.pop_back();
    const auto invalid_result = store_source_dataset(
        *project.store, invalid, "2026-08-17T11:40:03Z");
    expect_error("invalid property completeness rejects whole dataset",
                 invalid_result.status, StorageError::invalid_argument);
    expect_true("invalid dataset inserts no provenance",
                !source_exists(*project.store, "dataset.invalid"));
    expect_error("invalid dataset inserts no geometry marker",
                 list_source_geometry_index(*project.store, "dataset.invalid").status,
                 StorageError::record_not_found);
    expect_error("invalid dataset inserts no property marker",
                 list_source_feature_properties_index(*project.store, "dataset.invalid").status,
                 StorageError::record_not_found);
    expect_true("invalid dataset keeps revision one",
                project.store->metadata().revision == 1U);

    SourceDatasetRecord late = dataset("dataset.late", 'c', false);
    const auto geometry_only = store_source_dataset(
        *project.store, late, "2026-08-17T11:40:04Z");
    expect_true("geometry-only dataset stores", geometry_only.ok());
    expect_true("geometry-only dataset is revision two", project.store->metadata().revision == 2U);
    expect_error("geometry-only dataset leaves properties absent",
                 list_source_feature_properties_index(*project.store, "dataset.late").status,
                 StorageError::record_not_found);

    SourceDatasetRecord late_complete = dataset("dataset.late", 'c', true);
    const auto attached = store_source_dataset(
        *project.store, late_complete, "2026-08-17T11:40:05Z");
    expect_true("complete properties attach to existing immutable dataset",
                attached.ok() && attached.inserted && attached.durably_committed);
    expect_true("late property attach advances exactly one revision",
                project.store->metadata().revision == 3U);
    const auto late_properties = list_source_feature_properties_index(
        *project.store, "dataset.late");
    expect_true("late property attach becomes complete",
                late_properties.ok() && late_properties.features.size() == 2U);

    const auto geometry_retry_after_properties = store_source_dataset(
        *project.store, late, "2026-08-17T11:40:06Z");
    expect_true("geometry-only retry after property attach is non-destructive",
                geometry_retry_after_properties.ok() &&
                !geometry_retry_after_properties.inserted &&
                !geometry_retry_after_properties.durably_committed);
    expect_true("geometry-only retry preserves revision three",
                project.store->metadata().revision == 3U);
    expect_true("geometry-only retry preserves complete property marker",
                list_source_feature_properties_index(*project.store, "dataset.late").ok());

    SourceDatasetRecord conflict = late_complete;
    for (auto& feature : conflict.feature_properties->features) {
        if (feature.stable_id == "feature:a") {
            feature.properties.front().value = std::string("ZZ");
        }
    }
    const auto conflict_result = store_source_dataset(
        *project.store, conflict, "2026-08-17T11:40:07Z");
    expect_error("different complete properties conflict immutably",
                 conflict_result.status, StorageError::record_exists);
    expect_true("property conflict keeps revision three", project.store->metadata().revision == 3U);
    expect_true("atomic property datasets pass deep integrity", project.store->verify_integrity().ok());

    project.store.reset();
    std::filesystem::remove_all(root, ec);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
