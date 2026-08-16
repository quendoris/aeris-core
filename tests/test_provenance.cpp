// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#include "aeris/storage/geography.hpp"
#include "aeris/storage/project.hpp"
#include "aeris/storage/provenance.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {

int failures = 0;
constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr double kTwoPi = 2.0 * kPi;

void expect(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void expect_error(const aeris::storage::Status& status, const aeris::storage::StorageError expected, const char* message) {
    if (status.error != expected) {
        std::cerr << "FAIL: " << message << " expected=" << static_cast<int>(expected)
                  << " actual=" << static_cast<int>(status.error)
                  << " diagnostic=" << status.diagnostic << '\n';
        ++failures;
    }
}

std::string read_binary(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

aeris::storage::SourceSnapshotRecord natural_earth_record() {
    aeris::storage::SourceSnapshotRecord record;
    record.source_id = "source:land:baseline";
    record.adapter_id = "natural-earth.ne-110m-land.shapefile.v1";
    record.capability_bits = 1U;
    record.temporal_class = 1U;
    record.provider = "Natural Earth";
    record.dataset = "ne_110m_land";
    record.snapshot = "v5.1.2";
    record.dataset_version = "5.1.2";
    record.source_uri = "https://github.com/nvkelso/natural-earth-vector/releases/tag/v5.1.2";
    record.license_id = "LicenseRef-Natural-Earth-Public-Domain";
    record.content_sha256 = "1111111111111111111111111111111111111111111111111111111111111111";
    record.retrieved_at_utc = "2026-08-16T19:45:00Z";
    record.worldview = "";
    record.resources = {
        {"dataset.version", "3333333333333333333333333333333333333333333333333333333333333333", 6U},
        {"geometry.shp", "2222222222222222222222222222222222222222222222222222222222222222", 89504U},
        {"crs.prj", "4444444444444444444444444444444444444444444444444444444444444444", 147U},
    };
    return record;
}

aeris::storage::GeographicRingRecord ordinary_exterior_ring() {
    using namespace aeris::storage;
    GeographicRingRecord ring;
    ring.role = GeographicRingRole::exterior;
    ring.interior_side = GeographicInteriorSide::right;
    ring.closing_longitude_rad = -0.2;
    ring.vertices = {{-0.2, -0.1}, {0.2, -0.1}, {0.2, 0.1}, {-0.2, 0.1}};
    return ring;
}

aeris::storage::GeographicRingRecord ordinary_hole_ring() {
    using namespace aeris::storage;
    GeographicRingRecord ring;
    ring.role = GeographicRingRole::interior;
    ring.interior_side = GeographicInteriorSide::left;
    ring.closing_longitude_rad = -0.05;
    ring.vertices = {{-0.05, -0.05}, {-0.05, 0.05}, {0.05, 0.05}, {0.05, -0.05}};
    return ring;
}

aeris::storage::GeographicRingRecord winding_ring() {
    using namespace aeris::storage;
    GeographicRingRecord ring;
    ring.role = GeographicRingRole::exterior;
    ring.interior_side = GeographicInteriorSide::left;
    ring.longitude_winding = 1;
    ring.closing_longitude_rad = kTwoPi;
    ring.vertices = {{-0.0, 1.20}, {2.0, 1.24}, {4.0, 1.28}, {6.0, 1.22}};
    return ring;
}

aeris::storage::SourceDatasetRecord geographic_dataset() {
    using namespace aeris::storage;
    SourceDatasetRecord dataset;
    dataset.source = natural_earth_record();

    SourceFeatureRecord winding;
    winding.stable_id = "feature:z-winding";
    winding.source_id = "record:2";
    winding.rings = {winding_ring()};

    SourceFeatureRecord ordinary;
    ordinary.stable_id = "feature:a-ordinary";
    ordinary.source_id = "record:1";
    ordinary.rings = {ordinary_exterior_ring(), ordinary_hole_ring()};

    // Deliberately noncanonical input ordering: storage canonicalizes features
    // by stable ID while preserving ring order within each feature.
    dataset.features = {winding, ordinary};
    return dataset;
}

}  // namespace

int main() {
    using namespace aeris::storage;

    expect(is_canonical_sha256("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"),
           "lowercase SHA-256 syntax should be canonical");
    expect(!is_canonical_sha256("0123456789ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef"),
           "uppercase SHA-256 syntax should be rejected as noncanonical");
    expect(!is_canonical_sha256("abc"), "short SHA-256 syntax should be rejected");

    const std::filesystem::path root = std::filesystem::temp_directory_path() / "aeris-provenance-v0-test";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    if (ec) return 2;
    const std::filesystem::path project_path = root / "world.aeris";

    ProjectCreateOptions options;
    options.timestamp_utc = "2026-08-16T19:44:59Z";
    options.project_uuid = "11111111-2222-4333-8444-555555555555";
    auto project = ProjectStore::create(project_path, options);
    expect(project.ok(), "draft 0.3 project should create");
    if (!project.ok()) {
        std::cerr << project.status.diagnostic << '\n';
        return 1;
    }
    expect(project.store->metadata().format_major == 0 && project.store->metadata().format_minor == 3,
           "project metadata should advertise draft format 0.3");
    expect(project.store->metadata().revision == 0U, "empty project revision should start at zero");

    auto empty = list_source_snapshots(*project.store);
    expect(empty.ok() && empty.records.empty(), "new project source catalog should be empty");

    SourceDatasetRecord dataset = geographic_dataset();
    auto inserted = store_source_dataset(*project.store, dataset, "2026-08-16T19:45:01Z");
    expect(inserted.ok() && inserted.inserted && inserted.durably_committed,
           "source provenance and geographic features should commit atomically");
    expect(project.store->metadata().revision == 1U,
           "one source dataset mutation should advance project revision once");
    expect(project.store->metadata().modified_utc == "2026-08-16T19:45:01Z",
           "source dataset mutation should advance project modification timestamp");

    const std::string before_read = read_binary(project_path);
    auto listed = list_source_snapshots(*project.store);
    auto features = list_source_features(*project.store, dataset.source.source_id);
    const std::string after_read = read_binary(project_path);
    expect(listed.ok() && listed.records.size() == 1U, "stored source snapshot should enumerate");
    expect(features.ok() && features.records.size() == 2U, "stored geographic features should enumerate");
    expect(before_read == after_read, "read-only source/geography enumeration must not change project bytes");
    if (listed.ok() && listed.records.size() == 1U) {
        const auto& got = listed.records.front();
        expect(got.source_id == dataset.source.source_id && got.adapter_id == dataset.source.adapter_id,
               "stored source identity should round-trip");
        expect(got.provider == dataset.source.provider && got.dataset == dataset.source.dataset &&
                   got.snapshot == dataset.source.snapshot,
               "stored provenance identity should round-trip");
        expect(got.content_sha256 == dataset.source.content_sha256 &&
                   got.retrieved_at_utc == dataset.source.retrieved_at_utc,
               "stored content identity and acquisition time should round-trip");
        expect(got.resources.size() == 3U, "all source manifest resources should round-trip");
        if (got.resources.size() == 3U) {
            expect(got.resources[0].logical_name == "crs.prj" &&
                       got.resources[1].logical_name == "dataset.version" &&
                       got.resources[2].logical_name == "geometry.shp",
                   "resource enumeration should be deterministic by logical name");
        }
    }
    if (features.ok() && features.records.size() == 2U) {
        const auto& ordinary = features.records[0];
        const auto& winding = features.records[1];
        expect(ordinary.stable_id == "feature:a-ordinary" && winding.stable_id == "feature:z-winding",
               "feature enumeration should be deterministic by stable ID");
        expect(ordinary.rings.size() == 2U && ordinary.rings[0].role == GeographicRingRole::exterior &&
                   ordinary.rings[1].role == GeographicRingRole::interior,
               "ring order and exterior/interior roles should round-trip");
        expect(ordinary.rings[0].interior_side == GeographicInteriorSide::right &&
                   ordinary.rings[1].interior_side == GeographicInteriorSide::left,
               "ring interior-side topology should round-trip");
        expect(winding.rings.size() == 1U && winding.rings[0].longitude_winding == 1 &&
                   winding.rings[0].closing_longitude_rad == kTwoPi,
               "winding and closing longitude should round-trip exactly");
        if (winding.rings.size() == 1U && !winding.rings[0].vertices.empty()) {
            expect(winding.rings[0].vertices[0].longitude_rad == 0.0 &&
                       !std::signbit(winding.rings[0].vertices[0].longitude_rad),
                   "canonical BLOB should normalize negative zero to positive zero");
        }
    }

    const std::uint64_t revision_after_insert = project.store->metadata().revision;
    auto retry = store_source_dataset(*project.store, dataset, "2026-08-16T19:45:02Z");
    expect(retry.ok() && !retry.inserted && !retry.durably_committed,
           "exact source dataset retry should be idempotent");
    expect(project.store->metadata().revision == revision_after_insert,
           "idempotent dataset retry must not advance project revision");
    expect(project.store->metadata().modified_utc == "2026-08-16T19:45:01Z",
           "idempotent dataset retry must not advance project timestamp");

    SourceDatasetRecord geometry_conflict = dataset;
    geometry_conflict.features[0].rings[0].vertices[1].latitude_rad += 0.001;
    auto conflicting_geometry = store_source_dataset(
        *project.store,
        geometry_conflict,
        "2026-08-16T19:45:03Z");
    expect_error(conflicting_geometry.status, StorageError::record_exists,
                 "same source_id with changed immutable geography must fail closed");
    expect(project.store->metadata().revision == revision_after_insert,
           "conflicting geographic retry must not advance project revision");

    SourceDatasetRecord bad_ring = dataset;
    bad_ring.source.source_id = "source:bad-ring";
    bad_ring.features[0].rings[0].closing_longitude_rad = 0.0;
    auto rejected_ring = store_source_dataset(*project.store, bad_ring, "2026-08-16T19:45:04Z");
    expect_error(rejected_ring.status, StorageError::invalid_argument,
                 "noncanonical closing/winding relation should fail at API boundary");

    SourceDatasetRecord duplicate_feature = dataset;
    duplicate_feature.source.source_id = "source:duplicate-feature";
    duplicate_feature.features.push_back(duplicate_feature.features.front());
    auto rejected_feature = store_source_dataset(
        *project.store,
        duplicate_feature,
        "2026-08-16T19:45:05Z");
    expect_error(rejected_feature.status, StorageError::invalid_argument,
                 "duplicate stable feature IDs should fail at API boundary");

    SourceSnapshotRecord bad_hash = natural_earth_record();
    bad_hash.source_id = "source:bad-hash";
    bad_hash.content_sha256[0] = 'A';
    auto rejected_hash = store_source_snapshot(*project.store, bad_hash, "2026-08-16T19:45:06Z");
    expect_error(rejected_hash.status, StorageError::invalid_argument,
                 "noncanonical source content hash should fail at API boundary");

    SourceSnapshotRecord second = natural_earth_record();
    second.source_id = "source:admin0:snapshot";
    second.adapter_id = "example.admin0.v1";
    second.capability_bits = 2U;
    second.temporal_class = 2U;
    second.provider = "Example Provider";
    second.dataset = "admin0";
    second.snapshot = "2026-08-16";
    second.dataset_version = "2026.08";
    second.source_uri = "https://example.invalid/datasets/admin0/2026-08-16";
    second.license_id = "CC-BY-4.0";
    second.content_sha256 = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    second.retrieved_at_utc = "2026-08-16T19:45:07Z";
    second.worldview = "neutral-disputed";
    second.resources = {{"boundaries", "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", std::nullopt}};
    auto second_insert = store_source_snapshot(*project.store, second, "2026-08-16T19:45:08Z");
    expect(second_insert.ok() && second_insert.inserted && second_insert.durably_committed,
           "provenance-only low-level source snapshot should remain supported");
    expect(project.store->metadata().revision == 2U,
           "dataset plus independent provenance-only source should yield revision two");
    auto no_features = list_source_features(*project.store, second.source_id);
    expect(no_features.ok() && no_features.records.empty(),
           "provenance-only source should have an explicit empty geographic feature set");

    project.store.reset();
    auto reopened = ProjectStore::open(project_path);
    expect(reopened.ok(), "project with canonical geography should reopen");
    if (!reopened.ok()) {
        std::cerr << reopened.status.diagnostic << '\n';
        return 1;
    }
    auto after_reopen = list_source_snapshots(*reopened.store);
    auto geography_after_reopen = list_source_features(*reopened.store, dataset.source.source_id);
    expect(after_reopen.ok() && after_reopen.records.size() == 2U,
           "all committed source snapshots should survive close/reopen");
    expect(geography_after_reopen.ok() && geography_after_reopen.records.size() == 2U,
           "canonical geographic features should survive close/reopen");
    if (after_reopen.ok() && after_reopen.records.size() == 2U) {
        expect(after_reopen.records[0].source_id == "source:admin0:snapshot" &&
                   after_reopen.records[1].source_id == "source:land:baseline",
               "source enumeration should be deterministic by project source ID");
        expect(!after_reopen.records[0].resources[0].size_bytes.has_value(),
               "unknown verified resource byte length should round-trip as NULL");
    }
    expect(reopened.store->verify_integrity().ok(),
           "project with provenance/geography foreign keys should pass integrity verification");

    std::filesystem::remove_all(root, ec);
    return failures == 0 ? 0 : 1;
}
