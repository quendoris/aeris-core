// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/storage/feature_property.hpp"
#include "aeris/storage/geometry.hpp"
#include "aeris/storage/provenance.hpp"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
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

aeris::storage::SourceSnapshotRecord source_record() {
    aeris::storage::SourceSnapshotRecord source{};
    source.source_id = "world.admin0.fixture";
    source.adapter_id = "fixture.admin0.v1";
    source.capability_bits = 2U;
    source.temporal_class = 1U;
    source.provider = "Fixture Provider";
    source.dataset = "admin0";
    source.snapshot = "snapshot-1";
    source.dataset_version = "v1";
    source.source_uri = "fixture://admin0";
    source.license_id = "CC0-1.0";
    source.content_sha256 = std::string(64U, 'a');
    source.retrieved_at_utc = "2026-08-17T11:30:00Z";
    return source;
}

aeris::storage::GeographicRingRecord ring(double offset) {
    using namespace aeris::storage;
    GeographicRingRecord value{};
    value.role = StoredRingRole::exterior;
    value.interior_side = StoredInteriorSide::right;
    value.longitude_winding = 0;
    value.vertices = {
        {offset + 0.00, 0.00},
        {offset + 0.10, 0.00},
        {offset + 0.10, 0.10},
        {offset + 0.00, 0.10},
    };
    value.closing_longitude_rad = offset;
    return value;
}

aeris::storage::SourceGeometryRecord geometry_record() {
    using namespace aeris::storage;
    SourceGeometryRecord geometry{};
    geometry.source_id = "world.admin0.fixture";

    FeatureGeometryRecord b{};
    b.stable_id = "country:b";
    b.source_feature_id = "record:2";
    b.rings.push_back(ring(0.4));

    FeatureGeometryRecord a{};
    a.stable_id = "country:a";
    a.source_feature_id = "record:1";
    a.rings.push_back(ring(0.0));

    geometry.features.push_back(std::move(b));
    geometry.features.push_back(std::move(a));
    return geometry;
}

aeris::storage::SourceFeaturePropertiesRecord property_record() {
    using namespace aeris::storage;
    SourceFeaturePropertiesRecord properties{};
    properties.source_id = "world.admin0.fixture";

    FeaturePropertiesRecord b{};
    b.stable_id = "country:b";  // Explicit verified-empty property list.

    FeaturePropertiesRecord a{};
    a.stable_id = "country:a";
    a.properties.push_back({"weight", -0.0});
    a.properties.push_back({"name", std::string("Alpha")});
    a.properties.push_back({"iso_a2", std::string("AA")});
    a.properties.push_back({"admin_level", static_cast<std::int64_t>(-7)});
    a.properties.push_back({"claimed", true});
    a.properties.push_back({"empty_text", std::string{}});

    properties.features.push_back(std::move(b));
    properties.features.push_back(std::move(a));
    return properties;
}

}  // namespace

int main() {
    using namespace aeris::storage;

    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "aeris-feature-property-v0-test";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    expect_true("feature-property test directory creates", !ec);

    ProjectCreateOptions options{};
    options.timestamp_utc = "2026-08-17T11:29:59Z";
    options.project_uuid = "22345678-1234-4abc-8def-1234567890ab";
    const std::filesystem::path path = root / "world.aeris";
    auto project = ProjectStore::create(path, options);
    expect_true("feature-property project creates", project.ok());
    if (!project.ok()) return EXIT_FAILURE;
    expect_true("feature-property project advertises current draft",
                project.store->metadata().format_major == kDraftFormatMajor &&
                project.store->metadata().format_minor == kDraftFormatMinor);

    const SourceSnapshotRecord source = source_record();
    const auto source_store = store_source_snapshot(
        *project.store, source, "2026-08-17T11:30:01Z");
    expect_true("feature-property provenance stores", source_store.ok());
    SourceGeometryRecord geometry = geometry_record();
    const auto geometry_store = store_source_geometry(
        *project.store, geometry, "2026-08-17T11:30:02Z");
    expect_true("feature-property geometry stores", geometry_store.ok());
    expect_true("provenance plus geometry are two revisions", project.store->metadata().revision == 2U);

    const auto absent = list_source_feature_properties_index(*project.store, source.source_id);
    expect_error("absent properties differ from verified-empty",
                 absent.status, StorageError::record_not_found);

    SourceFeaturePropertiesRecord incomplete = property_record();
    incomplete.features.pop_back();
    const auto rejected_incomplete = store_source_feature_properties(
        *project.store, incomplete, "2026-08-17T11:30:03Z");
    expect_error("complete property set must match every geometry feature",
                 rejected_incomplete.status, StorageError::invalid_argument);
    expect_true("incomplete property set does not advance revision",
                project.store->metadata().revision == 2U);

    SourceFeaturePropertiesRecord properties = property_record();
    const auto stored = store_source_feature_properties(
        *project.store, properties, "2026-08-17T11:30:04Z");
    expect_true("complete feature properties commit",
                stored.ok() && stored.inserted && stored.durably_committed);
    expect_true("complete feature properties are one revision",
                project.store->metadata().revision == 3U);

    const auto index = list_source_feature_properties_index(*project.store, source.source_id);
    expect_true("property index enumerates every geometry feature",
                index.ok() && index.features.size() == 2U);
    if (index.ok() && index.features.size() == 2U) {
        expect_true("country:a sorts first with six properties",
                    index.features[0].stable_id == "country:a" &&
                    index.features[0].source_feature_id == "record:1" &&
                    index.features[0].property_count == 6U);
        expect_true("country:b remains explicitly verified-empty",
                    index.features[1].stable_id == "country:b" &&
                    index.features[1].property_count == 0U);
    }

    const auto loaded_a = load_feature_properties(*project.store, source.source_id, "country:a");
    expect_true("country:a properties load", loaded_a.ok() && loaded_a.properties.size() == 6U);
    if (loaded_a.ok() && loaded_a.properties.size() == 6U) {
        expect_true("properties sort canonically by key",
                    loaded_a.properties[0].key == "admin_level" &&
                    loaded_a.properties[1].key == "claimed" &&
                    loaded_a.properties[2].key == "empty_text" &&
                    loaded_a.properties[3].key == "iso_a2" &&
                    loaded_a.properties[4].key == "name" &&
                    loaded_a.properties[5].key == "weight");
        expect_true("negative int64 round-trips exactly",
                    std::get<std::int64_t>(loaded_a.properties[0].value) == -7);
        expect_true("bool round-trips exactly",
                    std::get<bool>(loaded_a.properties[1].value));
        expect_true("empty UTF-8 remains explicit empty BLOB text",
                    std::get<std::string>(loaded_a.properties[2].value).empty());
        const double weight = std::get<double>(loaded_a.properties[5].value);
        expect_true("negative zero canonicalizes to positive zero",
                    weight == 0.0 && !std::signbit(weight));
    }

    const auto loaded_b = load_feature_properties(*project.store, source.source_id, "country:b");
    expect_true("verified-empty country loads successfully",
                loaded_b.ok() && loaded_b.properties.empty());

    const auto retry = store_source_feature_properties(
        *project.store, properties, "2026-08-17T11:30:05Z");
    expect_true("exact feature-property retry is idempotent",
                retry.ok() && !retry.inserted && !retry.durably_committed);
    expect_true("exact retry keeps revision three", project.store->metadata().revision == 3U);

    SourceFeaturePropertiesRecord conflict = properties;
    for (auto& feature : conflict.features) {
        if (feature.stable_id == "country:a") {
            for (auto& property : feature.properties) {
                if (property.key == "name") property.value = std::string("Different");
            }
        }
    }
    const auto conflict_result = store_source_feature_properties(
        *project.store, conflict, "2026-08-17T11:30:06Z");
    expect_error("different immutable feature properties fail closed",
                 conflict_result.status, StorageError::record_exists);
    expect_true("property conflict keeps revision three", project.store->metadata().revision == 3U);
    expect_true("valid property project passes deep integrity", project.store->verify_integrity().ok());

    project.store.reset();
    auto reopened = ProjectStore::open(path);
    expect_true("property-bearing project reopens", reopened.ok());
    if (!reopened.ok()) return EXIT_FAILURE;
    const auto reopened_b = load_feature_properties(*reopened.store, source.source_id, "country:b");
    expect_true("verified-empty property state survives reopen",
                reopened_b.ok() && reopened_b.properties.empty());
    expect_true("reopened property project passes integrity", reopened.store->verify_integrity().ok());

    sqlite3* raw = nullptr;
    const std::string sqlite_path = path.string();
    const int open_rc = sqlite3_open_v2(sqlite_path.c_str(), &raw, SQLITE_OPEN_READWRITE, nullptr);
    expect_true("hostile property database opens", open_rc == SQLITE_OK && raw != nullptr);
    if (open_rc == SQLITE_OK && raw != nullptr) {
        char* error = nullptr;
        const int rc = sqlite3_exec(
            raw,
            "UPDATE aeris_feature_property SET value_payload=X'000000000000F87F' "
            "WHERE source_id='world.admin0.fixture' AND stable_id='country:a' AND property_key='weight';",
            nullptr, nullptr, &error);
        if (error != nullptr) sqlite3_free(error);
        expect_true("hostile NaN property payload writes below API", rc == SQLITE_OK);
        sqlite3_close(raw);
        raw = nullptr;
    }
    const Status hostile = reopened.store->verify_integrity();
    expect_error("deep integrity rejects hostile NaN feature property",
                 hostile, StorageError::schema_invalid);

    reopened.store.reset();
    std::filesystem::remove_all(root, ec);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
