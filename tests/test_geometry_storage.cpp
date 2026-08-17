// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#include "aeris/storage/geometry.hpp"
#include "aeris/storage/provenance.hpp"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
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

std::string read_binary(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

aeris::storage::SourceSnapshotRecord provenance_record(
    std::string source_id,
    std::string snapshot) {
    aeris::storage::SourceSnapshotRecord record{};
    record.source_id = std::move(source_id);
    record.adapter_id = "fixture.adapter.v1";
    record.capability_bits = 1U;
    record.temporal_class = 1U;
    record.provider = "fixture-provider";
    record.dataset = "fixture-dataset";
    record.snapshot = std::move(snapshot);
    record.dataset_version = "fixture-v1";
    record.source_uri = "fixture://canonical-geometry";
    record.license_id = "CC0-1.0";
    record.content_sha256 = std::string(64U, 'a');
    record.retrieved_at_utc = "2026-08-17T08:10:00Z";
    return record;
}

aeris::storage::GeographicRingRecord local_ring() {
    using namespace aeris::storage;
    GeographicRingRecord ring{};
    ring.role = StoredRingRole::exterior;
    ring.interior_side = StoredInteriorSide::right;
    ring.longitude_winding = 0;
    ring.closing_longitude_rad = -0.0;
    ring.vertices = {
        {-0.0, -0.0},
        {0.20, 0.00},
        {0.20, 0.20},
        {0.00, 0.20},
    };
    return ring;
}

aeris::storage::GeographicRingRecord winding_ring() {
    using namespace aeris::storage;
    constexpr double two_pi = 6.283185307179586476925286766559005768;
    GeographicRingRecord ring{};
    ring.role = StoredRingRole::exterior;
    ring.interior_side = StoredInteriorSide::left;
    ring.longitude_winding = 1;
    ring.closing_longitude_rad = two_pi;
    ring.vertices = {
        {0.0, 0.0},
        {1.5, 0.2},
        {3.0, 0.0},
        {4.5, -0.2},
        {6.0, 0.0},
    };
    return ring;
}

aeris::storage::SourceGeometryRecord geometry_record(const std::string& source_id) {
    using namespace aeris::storage;
    SourceGeometryRecord record{};
    record.source_id = source_id;

    FeatureGeometryRecord later{};
    later.stable_id = "feature:b";
    later.source_feature_id = "record:2";
    later.rings.push_back(winding_ring());

    FeatureGeometryRecord earlier{};
    earlier.stable_id = "feature:a";
    earlier.source_feature_id = "record:1";
    earlier.rings.push_back(local_ring());

    // Deliberately reverse canonical stable-id order. Storage must canonicalize
    // feature order without changing ring order inside a feature.
    record.features.push_back(std::move(later));
    record.features.push_back(std::move(earlier));
    return record;
}

}  // namespace

int main() {
    using namespace aeris::storage;

    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "aeris-geometry-storage-contract";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    ec.clear();
    std::filesystem::create_directories(root, ec);
    expect(!ec, "geometry test directory should be creatable");

    const std::filesystem::path project_path = root / "world.aeris";
    ProjectCreateOptions options{};
    options.timestamp_utc = "2026-08-17T08:09:00Z";
    options.project_uuid = "12345678-1234-4abc-8def-1234567890ab";
    options.producer = "aeris-geometry-test";
    options.producer_version = "0.4-test";

    auto created = ProjectStore::create(project_path, options);
    expect(created.ok(), "draft 0.4 project should create");
    if (!created.ok()) {
        std::cerr << created.status.diagnostic << '\n';
        return EXIT_FAILURE;
    }
    expect(created.store->metadata().format_minor == 4, "draft format minor should be 4");
    expect(created.store->metadata().revision == 0U, "new geometry project revision should begin at zero");

    auto source = provenance_record("world.land.primary", "snapshot-1");
    auto stored_source = store_source_snapshot(*created.store, source, "2026-08-17T08:10:01Z");
    expect(stored_source.ok() && stored_source.inserted && stored_source.durably_committed,
           "source provenance should commit before geometry");
    expect(created.store->metadata().revision == 1U, "source provenance should advance revision once");

    SourceGeometryRecord geometry = geometry_record(source.source_id);
    auto stored = store_source_geometry(*created.store, geometry, "2026-08-17T08:10:02Z");
    expect(stored.ok() && stored.inserted && stored.durably_committed,
           "canonical geometry should commit");
    expect(created.store->metadata().revision == 2U, "geometry should advance project revision exactly once");

    const std::string bytes_before_reads = read_binary(project_path);
    auto index = list_source_geometry_index(*created.store, source.source_id);
    expect(index.ok(), "geometry index should enumerate");
    expect(index.features.size() == 2U, "geometry index should contain two features");
    if (index.features.size() == 2U) {
        expect(index.features[0].stable_id == "feature:a", "geometry index should be stable-id canonical");
        expect(index.features[0].source_feature_id == "record:1", "feature:a source identity should survive");
        expect(index.features[0].ring_count == 1U, "feature:a ring count should survive");
        expect(index.features[1].stable_id == "feature:b", "feature:b should sort second");
    }

    auto feature_a = load_feature_geometry(*created.store, source.source_id, "feature:a");
    expect(feature_a.ok(), "feature:a should load lazily");
    if (feature_a.ok()) {
        expect(feature_a.feature->rings.size() == 1U, "feature:a should have one ring");
        const auto& ring = feature_a.feature->rings.front();
        expect(ring.role == StoredRingRole::exterior, "ring role should round-trip");
        expect(ring.interior_side == StoredInteriorSide::right, "interior side should round-trip");
        expect(ring.longitude_winding == 0, "local ring winding should round-trip");
        expect(!std::signbit(ring.closing_longitude_rad), "closing -0 must canonicalize to +0");
        expect(!std::signbit(ring.vertices.front().longitude_rad), "longitude -0 must canonicalize to +0");
        expect(!std::signbit(ring.vertices.front().latitude_rad), "latitude -0 must canonicalize to +0");
    }

    auto feature_b = load_feature_geometry(*created.store, source.source_id, "feature:b");
    expect(feature_b.ok(), "feature:b should load lazily");
    if (feature_b.ok()) {
        const auto& ring = feature_b.feature->rings.front();
        expect(ring.longitude_winding == 1, "nonzero longitude winding should survive disk");
        expect(ring.interior_side == StoredInteriorSide::left, "winding ring interior side should survive disk");
        expect(ring.vertices.size() == 5U, "winding ring vertices should survive disk");
    }

    expect(read_binary(project_path) == bytes_before_reads,
           "read-only geometry index/load must not rewrite project bytes");

    auto retry = store_source_geometry(*created.store, geometry, "2026-08-17T08:10:03Z");
    expect(retry.ok() && !retry.inserted && !retry.durably_committed,
           "exact geometry retry should be idempotent");
    expect(created.store->metadata().revision == 2U, "idempotent geometry retry must not advance revision");

    SourceGeometryRecord conflict = geometry;
    conflict.features.front().rings.front().vertices[1].latitude_rad += 0.01;
    auto conflicting = store_source_geometry(*created.store, conflict, "2026-08-17T08:10:04Z");
    expect_error(conflicting.status, StorageError::record_exists,
                 "different geometry under one immutable source ID must fail");
    expect(created.store->metadata().revision == 2U, "conflicting geometry must not advance revision");

    SourceGeometryRecord missing = geometry_record("missing.source");
    auto missing_parent = store_source_geometry(*created.store, missing, "2026-08-17T08:10:05Z");
    expect_error(missing_parent.status, StorageError::record_not_found,
                 "geometry without persisted provenance parent must fail closed");
    expect(created.store->metadata().revision == 2U, "missing-parent geometry must not advance revision");

    auto no_feature = load_feature_geometry(*created.store, source.source_id, "feature:missing");
    expect_error(no_feature.status, StorageError::record_not_found,
                 "missing feature stable ID should be explicit");

    auto empty_source = provenance_record("world.empty.fixture", "snapshot-empty");
    empty_source.content_sha256 = std::string(64U, 'b');
    auto stored_empty_source = store_source_snapshot(
        *created.store, empty_source, "2026-08-17T08:10:06Z");
    expect(stored_empty_source.ok(), "empty-geometry fixture provenance should commit");
    expect(created.store->metadata().revision == 3U, "second provenance should advance revision");

    SourceGeometryRecord empty_geometry{};
    empty_geometry.source_id = empty_source.source_id;
    auto stored_empty = store_source_geometry(
        *created.store, empty_geometry, "2026-08-17T08:10:07Z");
    expect(stored_empty.ok() && stored_empty.inserted && stored_empty.durably_committed,
           "explicit empty geometry set should be persistable");
    expect(created.store->metadata().revision == 4U, "empty geometry marker is a semantic mutation");
    auto empty_index = list_source_geometry_index(*created.store, empty_source.source_id);
    expect(empty_index.ok() && empty_index.features.empty(),
           "empty geometry marker must distinguish recorded-empty from missing geometry");
    auto empty_retry = store_source_geometry(
        *created.store, empty_geometry, "2026-08-17T08:10:08Z");
    expect(empty_retry.ok() && !empty_retry.inserted && !empty_retry.durably_committed,
           "empty geometry retry should be idempotent");
    expect(created.store->metadata().revision == 4U, "empty retry must not advance revision");

    SourceGeometryRecord invalid = geometry;
    invalid.source_id = empty_source.source_id;
    invalid.features.front().rings.front().longitude_winding = 2;
    auto invalid_winding = store_source_geometry(
        *created.store, invalid, "2026-08-17T08:10:09Z");
    expect_error(invalid_winding.status, StorageError::invalid_argument,
                 "closing longitude and winding disagreement must be rejected before mutation");
    expect(created.store->metadata().revision == 4U, "invalid geometry must not advance revision");

    expect(created.store->verify_integrity().ok(), "geometry-bearing project should pass project integrity surface checks");
    created.store.reset();

    auto reopened = ProjectStore::open(project_path);
    expect(reopened.ok(), "geometry-bearing project should reopen after close");
    if (reopened.ok()) {
        expect(reopened.store->metadata().revision == 4U, "geometry revisions should survive reopen");
        auto reopened_feature = load_feature_geometry(*reopened.store, source.source_id, "feature:b");
        expect(reopened_feature.ok(), "canonical feature should survive close/reopen");
        if (reopened_feature.ok()) {
            expect(reopened_feature.feature->rings.front().longitude_winding == 1,
                   "winding topology should survive close/reopen");
        }
    }

    std::filesystem::remove_all(root, ec);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
