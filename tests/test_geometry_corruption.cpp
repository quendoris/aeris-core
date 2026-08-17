// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#include "aeris/storage/geometry.hpp"
#include "aeris/storage/provenance.hpp"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

#include <sqlite3.h>

namespace {

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void expect_schema_invalid(
    const aeris::storage::Status& status,
    const std::string_view message) {
    if (status.error != aeris::storage::StorageError::schema_invalid) {
        ++failures;
        std::cerr << "FAIL: " << message
                  << " actual=" << static_cast<int>(status.error)
                  << " diagnostic=" << status.diagnostic << '\n';
    }
}

struct SqliteCloser final {
    void operator()(sqlite3* db) const noexcept {
        if (db != nullptr) sqlite3_close(db);
    }
};
using DbPtr = std::unique_ptr<sqlite3, SqliteCloser>;

struct BlobCloser final {
    void operator()(sqlite3_blob* blob) const noexcept {
        if (blob != nullptr) sqlite3_blob_close(blob);
    }
};
using BlobPtr = std::unique_ptr<sqlite3_blob, BlobCloser>;

bool exec_sql(sqlite3* db, const char* sql) {
    char* message = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &message);
    if (rc != SQLITE_OK) {
        std::cerr << "sqlite fixture mutation failed: "
                  << (message != nullptr ? message : sqlite3_errmsg(db)) << '\n';
        sqlite3_free(message);
        return false;
    }
    return true;
}

DbPtr open_raw(const std::filesystem::path& path) {
    sqlite3* raw = nullptr;
    const std::string utf8_path = path.u8string();
    if (sqlite3_open_v2(
            utf8_path.c_str(),
            &raw,
            SQLITE_OPEN_READWRITE,
            nullptr) != SQLITE_OK) {
        if (raw != nullptr) {
            std::cerr << "raw sqlite open failed: " << sqlite3_errmsg(raw) << '\n';
            sqlite3_close(raw);
        }
        return {};
    }
    return DbPtr(raw);
}

bool create_fixture(const std::filesystem::path& path, const std::string& uuid) {
    using namespace aeris::storage;

    ProjectCreateOptions options{};
    options.timestamp_utc = "2026-08-17T08:20:00Z";
    options.project_uuid = uuid;
    auto project = ProjectStore::create(path, options);
    if (!project.ok()) {
        std::cerr << "fixture project create failed: " << project.status.diagnostic << '\n';
        return false;
    }

    SourceSnapshotRecord source{};
    source.source_id = "source.geometry.corruption";
    source.adapter_id = "fixture.adapter.v1";
    source.capability_bits = 1U;
    source.temporal_class = 1U;
    source.provider = "fixture-provider";
    source.dataset = "fixture-dataset";
    source.snapshot = "snapshot-1";
    source.dataset_version = "fixture-v1";
    source.source_uri = "fixture://geometry-corruption";
    source.license_id = "CC0-1.0";
    source.content_sha256 = std::string(64U, 'c');
    source.retrieved_at_utc = "2026-08-17T08:20:01Z";
    const auto provenance = store_source_snapshot(
        *project.store, source, "2026-08-17T08:20:02Z");
    if (!provenance.ok()) {
        std::cerr << "fixture provenance failed: " << provenance.status.diagnostic << '\n';
        return false;
    }

    SourceGeometryRecord geometry{};
    geometry.source_id = source.source_id;
    FeatureGeometryRecord feature{};
    feature.stable_id = "feature:fixture";
    feature.source_feature_id = "record:1";
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
    geometry.features.push_back(std::move(feature));

    const auto stored = store_source_geometry(
        *project.store, geometry, "2026-08-17T08:20:03Z");
    if (!stored.ok()) {
        std::cerr << "fixture geometry failed: " << stored.status.diagnostic << '\n';
        return false;
    }
    project.store.reset();
    return true;
}

void test_wrong_model_id(const std::filesystem::path& root) {
    using namespace aeris::storage;
    const auto path = root / "wrong-model.aeris";
    if (!create_fixture(path, "10000000-0000-4000-8000-000000000001")) {
        ++failures;
        return;
    }
    auto db = open_raw(path);
    expect(db != nullptr, "raw wrong-model fixture should open");
    if (!db || !exec_sql(db.get(), "UPDATE aeris_source_geometry SET model_id='broken.geometry.model';")) {
        ++failures;
        return;
    }
    db.reset();

    auto project = ProjectStore::open(path);
    expect(project.ok(), "structurally valid wrong-model project should pass container open");
    if (!project.ok()) return;
    expect_schema_invalid(
        project.store->verify_integrity(),
        "deep project audit must reject unknown geometry model ID");
    const auto index = list_source_geometry_index(*project.store, "source.geometry.corruption");
    expect_schema_invalid(index.status, "unknown geometry model ID must fail semantic read");
}

void test_sparse_ring_index(const std::filesystem::path& root) {
    using namespace aeris::storage;
    const auto path = root / "sparse-index.aeris";
    if (!create_fixture(path, "20000000-0000-4000-8000-000000000002")) {
        ++failures;
        return;
    }
    auto db = open_raw(path);
    expect(db != nullptr, "raw sparse-index fixture should open");
    if (!db || !exec_sql(db.get(), "UPDATE aeris_feature_ring SET ring_index=2 WHERE ring_index=0;")) {
        ++failures;
        return;
    }
    db.reset();

    auto project = ProjectStore::open(path);
    expect(project.ok(), "sparse ring-index project should remain SQLite-structurally valid");
    if (!project.ok()) return;
    expect_schema_invalid(
        project.store->verify_integrity(),
        "deep project audit must reject noncontiguous ring indices");
    const auto feature = load_feature_geometry(
        *project.store, "source.geometry.corruption", "feature:fixture");
    expect_schema_invalid(feature.status, "noncontiguous ring indices must fail semantic read");
}

void test_nonfinite_coordinate_blob(const std::filesystem::path& root) {
    using namespace aeris::storage;
    const auto path = root / "nan-coordinate.aeris";
    if (!create_fixture(path, "30000000-0000-4000-8000-000000000003")) {
        ++failures;
        return;
    }
    auto db = open_raw(path);
    expect(db != nullptr, "raw NaN fixture should open");
    if (!db) return;

    sqlite3_stmt* raw_stmt = nullptr;
    if (sqlite3_prepare_v2(
            db.get(),
            "SELECT rowid FROM aeris_feature_ring LIMIT 1;",
            -1,
            &raw_stmt,
            nullptr) != SQLITE_OK) {
        ++failures;
        std::cerr << "could not locate geometry BLOB rowid\n";
        return;
    }
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(raw_stmt, &sqlite3_finalize);
    if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
        ++failures;
        std::cerr << "geometry BLOB rowid query returned no row\n";
        return;
    }
    const sqlite3_int64 rowid = sqlite3_column_int64(stmt.get(), 0);
    stmt.reset();

    sqlite3_blob* raw_blob = nullptr;
    if (sqlite3_blob_open(
            db.get(),
            "main",
            "aeris_feature_ring",
            "vertices_f64le",
            rowid,
            1,
            &raw_blob) != SQLITE_OK) {
        ++failures;
        std::cerr << "could not open coordinate BLOB for hostile fixture\n";
        return;
    }
    BlobPtr blob(raw_blob);
    const std::array<unsigned char, 8> quiet_nan_le{
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0xf8U, 0x7fU,
    };
    if (sqlite3_blob_write(
            blob.get(), quiet_nan_le.data(),
            static_cast<int>(quiet_nan_le.size()), 0) != SQLITE_OK) {
        ++failures;
        std::cerr << "could not inject NaN into coordinate BLOB\n";
        return;
    }
    blob.reset();
    db.reset();

    auto project = ProjectStore::open(path);
    expect(project.ok(), "NaN coordinate project should remain SQLite-structurally valid");
    if (!project.ok()) return;
    expect_schema_invalid(
        project.store->verify_integrity(),
        "deep project audit must reject nonfinite canonical coordinates");
    const auto feature = load_feature_geometry(
        *project.store, "source.geometry.corruption", "feature:fixture");
    expect_schema_invalid(feature.status, "nonfinite binary64 coordinate must fail semantic read");
}

}  // namespace

int main() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "aeris-geometry-corruption-contract";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    ec.clear();
    std::filesystem::create_directories(root, ec);
    expect(!ec, "geometry corruption test root should be creatable");
    if (ec) return EXIT_FAILURE;

    test_wrong_model_id(root);
    test_sparse_ring_index(root);
    test_nonfinite_coordinate_blob(root);

    std::filesystem::remove_all(root, ec);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
