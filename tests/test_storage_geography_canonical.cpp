// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/storage/geography.hpp"
#include "aeris/storage/project.hpp"
#include "aeris/storage/provenance.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string_view>

namespace {

int failures = 0;

void expect_true(const std::string_view name, const bool condition) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL " << name << '\n';
    }
}

[[nodiscard]] aeris::storage::SourceDatasetRecord base_dataset(const std::string_view source_id) {
    using namespace aeris::storage;

    SourceDatasetRecord dataset{};
    dataset.source.source_id = std::string(source_id);
    dataset.source.adapter_id = "fixture.storage-geography.v1";
    dataset.source.capability_bits = 1U;
    dataset.source.temporal_class = 0U;
    dataset.source.provider = "fixture-provider";
    dataset.source.dataset = "fixture-dataset";
    dataset.source.snapshot = "fixture-snapshot";
    dataset.source.dataset_version = "1";
    dataset.source.source_uri = "fixture://storage-geography";
    dataset.source.license_id = "CC0-1.0";
    dataset.source.content_sha256 =
        "1111111111111111111111111111111111111111111111111111111111111111";
    dataset.source.retrieved_at_utc = "2026-08-16T21:10:00Z";

    SourceFeatureRecord feature{};
    feature.stable_id = "feature:one";
    feature.source_id = "record:1";

    GeographicRingRecord ring{};
    ring.role = GeographicRingRole::exterior;
    ring.interior_side = GeographicInteriorSide::right;
    ring.closing_longitude_rad = -0.2;
    ring.longitude_winding = 0;
    ring.vertices = {
        {-0.2, -0.1},
        {0.2, -0.1},
        {0.2, 0.1},
        {-0.2, 0.1},
    };
    feature.rings.push_back(std::move(ring));
    dataset.features.push_back(std::move(feature));
    return dataset;
}

}  // namespace

int main() {
    using namespace aeris::storage;

    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "aeris-storage-geography-canonical-test";
    std::error_code ec{};
    std::filesystem::remove_all(root, ec);
    ec.clear();
    std::filesystem::create_directories(root, ec);
    if (ec) return EXIT_FAILURE;

    ProjectCreateOptions create{};
    create.timestamp_utc = "2026-08-16T21:10:00Z";
    create.project_uuid = "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee";
    auto project = ProjectStore::create(root / "world.aeris", create);
    expect_true("project creates", project.ok());
    if (!project.ok()) {
        std::filesystem::remove_all(root, ec);
        return EXIT_FAILURE;
    }

    SourceDatasetRecord duplicate_close = base_dataset("source:duplicate-close");
    duplicate_close.features.front().rings.front().vertices.push_back(
        duplicate_close.features.front().rings.front().vertices.front());
    const SourceDatasetMutationResult duplicate_result = store_source_dataset(
        *project.store,
        duplicate_close,
        "2026-08-16T21:10:01Z");
    expect_true(
        "explicit terminal closing vertex rejected by low-level storage",
        duplicate_result.status.error == StorageError::invalid_argument);
    expect_true("duplicate closure is not inserted", !duplicate_result.inserted);
    expect_true("duplicate closure is not committed", !duplicate_result.durably_committed);
    expect_true("duplicate closure keeps revision zero", project.store->metadata().revision == 0U);
    const auto after_duplicate = list_source_snapshots(*project.store);
    expect_true(
        "duplicate closure leaves source catalog empty",
        after_duplicate.ok() && after_duplicate.records.empty());

    SourceDatasetRecord hostile_closing = base_dataset("source:hostile-closing");
    hostile_closing.features.front().rings.front().closing_longitude_rad =
        std::numeric_limits<double>::max();
    const SourceDatasetMutationResult hostile_result = store_source_dataset(
        *project.store,
        hostile_closing,
        "2026-08-16T21:10:02Z");
    expect_true(
        "extreme finite closing longitude rejected without mutation",
        hostile_result.status.error == StorageError::invalid_argument);
    expect_true("hostile closing is not inserted", !hostile_result.inserted);
    expect_true("hostile closing is not committed", !hostile_result.durably_committed);
    expect_true("hostile closing keeps revision zero", project.store->metadata().revision == 0U);

    const auto final_sources = list_source_snapshots(*project.store);
    expect_true(
        "hostile low-level geography leaves source catalog empty",
        final_sources.ok() && final_sources.records.empty());
    expect_true("project integrity remains valid", project.store->verify_integrity().ok());

    project.store.reset();
    std::filesystem::remove_all(root, ec);

    if (failures != 0) {
        std::cerr << failures << " storage geography assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "storage_geography_canonical: PASS\n";
    return EXIT_SUCCESS;
}
