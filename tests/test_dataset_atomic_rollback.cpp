// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#include "aeris/storage/dataset.hpp"

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

struct SqliteCloser final {
    void operator()(sqlite3* db) const noexcept {
        if (db != nullptr) sqlite3_close(db);
    }
};
using DbPtr = std::unique_ptr<sqlite3, SqliteCloser>;

DbPtr open_raw(const std::filesystem::path& path) {
    sqlite3* raw = nullptr;
    const std::string utf8_path = path.u8string();
    if (sqlite3_open_v2(utf8_path.c_str(), &raw, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
        if (raw != nullptr) sqlite3_close(raw);
        return {};
    }
    return DbPtr(raw);
}

bool exec_sql(sqlite3* db, const char* sql) {
    char* message = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &message);
    if (rc != SQLITE_OK) {
        std::cerr << "fixture SQL failed: "
                  << (message != nullptr ? message : sqlite3_errmsg(db)) << '\n';
        sqlite3_free(message);
        return false;
    }
    return true;
}

aeris::storage::SourceDatasetRecord dataset_record() {
    using namespace aeris::storage;
    SourceDatasetRecord dataset{};
    dataset.provenance.source_id = "world.rollback.after-provenance";
    dataset.provenance.adapter_id = "fixture.rollback.v1";
    dataset.provenance.capability_bits = 1U;
    dataset.provenance.temporal_class = 1U;
    dataset.provenance.provider = "fixture-provider";
    dataset.provenance.dataset = "fixture-dataset";
    dataset.provenance.snapshot = "snapshot-1";
    dataset.provenance.dataset_version = "fixture-v1";
    dataset.provenance.source_uri = "fixture://dataset-rollback";
    dataset.provenance.license_id = "CC0-1.0";
    dataset.provenance.content_sha256 =
        "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee";
    dataset.provenance.retrieved_at_utc = "2026-08-17T09:20:01Z";

    dataset.geometry.source_id = dataset.provenance.source_id;
    FeatureGeometryRecord feature{};
    feature.stable_id = "feature:rollback";
    feature.source_feature_id = "record:rollback";
    GeographicRingRecord ring{};
    ring.role = StoredRingRole::exterior;
    ring.interior_side = StoredInteriorSide::right;
    ring.vertices = {
        {0.0, 0.0},
        {0.1, 0.0},
        {0.1, 0.1},
        {0.0, 0.1},
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
        std::filesystem::temp_directory_path() / "aeris-dataset-atomic-rollback";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    ec.clear();
    std::filesystem::create_directories(root, ec);
    expect(!ec, "rollback fixture directory should be creatable");
    if (ec) return EXIT_FAILURE;

    const auto path = root / "world.aeris";
    ProjectCreateOptions options{};
    options.timestamp_utc = "2026-08-17T09:20:00Z";
    options.project_uuid = "80000000-0000-4000-8000-000000000008";
    auto created = ProjectStore::create(path, options);
    expect(created.ok(), "rollback fixture project should create");
    if (!created.ok()) return EXIT_FAILURE;

    // Corrupt only a later geometry table after the ProjectStore has already
    // accepted the container. The combined mutation can still validate the
    // project, begin its transaction, and insert provenance rows before it
    // reaches the deliberately missing ring table.
    auto raw = open_raw(path);
    expect(raw != nullptr, "raw rollback fixture should open");
    if (!raw || !exec_sql(raw.get(), "PRAGMA foreign_keys=OFF; DROP TABLE aeris_feature_ring;")) {
        return EXIT_FAILURE;
    }
    raw.reset();

    const auto result = store_source_dataset(
        *created.store, dataset_record(), "2026-08-17T09:20:02Z");
    expect(!result.ok(), "combined dataset should fail after later geometry SQL is unavailable");
    expect(result.status.error == StorageError::sqlite_failure,
           "late geometry failure should surface as SQLite failure");
    expect(!result.inserted && !result.durably_committed,
           "failed combined transaction must not report durable insertion");
    expect(created.store->metadata().revision == 0U,
           "late combined failure must leave in-memory project revision unchanged");

    const auto sources = list_source_snapshots(*created.store);
    expect(sources.ok(), "provenance catalog should remain readable after transaction rollback");
    expect(sources.records.empty(),
           "provenance rows inserted before late geometry failure must be rolled back");

    // Read the canonical metadata through a fresh project-independent SQLite
    // query as a second proof that no revision update escaped the transaction.
    raw = open_raw(path);
    expect(raw != nullptr, "raw rollback verification should reopen");
    if (raw) {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(raw.get(), "SELECT revision FROM aeris_meta WHERE id=1;", -1, &stmt, nullptr) != SQLITE_OK) {
            ++failures;
        } else {
            std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> guard(stmt, &sqlite3_finalize);
            expect(sqlite3_step(guard.get()) == SQLITE_ROW,
                   "rollback verification should read project revision");
            expect(sqlite3_column_int64(guard.get(), 0) == 0,
                   "on-disk project revision must remain zero after rollback");
        }
    }

    std::filesystem::remove_all(root, ec);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
