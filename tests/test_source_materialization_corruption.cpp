// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/storage/provenance.hpp"

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

void expect_schema_invalid(
    const std::string_view name,
    const aeris::storage::Status& status) {
    if (status.error != aeris::storage::StorageError::schema_invalid) {
        ++failures;
        std::cerr << "FAIL " << name
                  << " expected schema_invalid actual="
                  << static_cast<int>(status.error)
                  << " diagnostic=" << status.diagnostic << '\n';
    }
}

class Fixture final {
public:
    explicit Fixture(std::string suffix) {
        root_ = std::filesystem::temp_directory_path() /
                ("aeris-source-state-corruption-" + std::move(suffix));
        std::error_code ignored{};
        std::filesystem::remove_all(root_, ignored);
        std::filesystem::create_directories(root_);

        aeris::storage::ProjectCreateOptions options{};
        options.timestamp_utc = "2026-08-19T08:10:00Z";
        options.project_uuid = uuid_for_root();
        auto created = aeris::storage::ProjectStore::create(path(), options);
        if (created.ok()) project_ = std::move(created.store);
    }

    ~Fixture() {
        project_.reset();
        std::error_code ignored{};
        std::filesystem::remove_all(root_, ignored);
    }

    [[nodiscard]] aeris::storage::ProjectStore* project() noexcept {
        return project_.get();
    }
    [[nodiscard]] std::filesystem::path path() const {
        return root_ / "world.aeris";
    }

private:
    [[nodiscard]] std::string uuid_for_root() const {
        const bool first = root_.filename().string().find("referenced-geometry") != std::string::npos;
        return first
            ? "aaaaaaaa-1111-4aaa-8aaa-111111111111"
            : "bbbbbbbb-2222-4bbb-8bbb-222222222222";
    }

    std::filesystem::path root_;
    std::unique_ptr<aeris::storage::ProjectStore> project_;
};

[[nodiscard]] aeris::storage::SourceSnapshotRecord reference_record(
    const std::string& source_id) {
    aeris::storage::SourceSnapshotRecord record{};
    record.source_id = source_id;
    record.adapter_id = "fixture.vector.v1";
    record.capability_bits = 1U;
    record.temporal_class = 0U;
    record.provider = "Fixture";
    record.dataset = "corruption";
    record.snapshot = "v1";
    record.dataset_version = "1";
    record.source_uri = "fixture://corruption";
    record.license_id = "CC0-1.0";
    record.content_sha256 = std::string(64U, 'e');
    record.worldview = "fixture";
    return record;
}

[[nodiscard]] bool raw_exec(
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
    return rc == SQLITE_OK;
}

void test_referenced_source_with_geometry_is_rejected() {
    using namespace aeris::storage;
    Fixture fixture("referenced-geometry");
    ProjectStore* project = fixture.project();
    expect_true("referenced corruption fixture creates", project != nullptr);
    if (project == nullptr) return;

    const auto source = store_source_snapshot(
        *project,
        reference_record("source.referenced.corrupt"),
        "2026-08-19T08:10:01Z");
    expect_true("referenced corruption source stores", source.ok());

    expect_true(
        "hostile geometry marker writes below revision boundary",
        raw_exec(
            fixture.path(),
            "INSERT INTO aeris_source_geometry(source_id,model_id,encoding_id,feature_count) "
            "VALUES('source.referenced.corrupt','aeris.geometry.wgs84-linear-ring.v1',"
            "'aeris.coord.ieee754-binary64-le-radians.v1',0);"));

    expect_schema_invalid(
        "deep audit rejects referenced source owning geometry",
        project->verify_integrity());
}

void test_materialized_source_without_geometry_is_rejected() {
    using namespace aeris::storage;
    Fixture fixture("materialized-no-geometry");
    ProjectStore* project = fixture.project();
    expect_true("materialized corruption fixture creates", project != nullptr);
    if (project == nullptr) return;

    const auto source = store_source_snapshot(
        *project,
        reference_record("source.materialized.corrupt"),
        "2026-08-19T08:11:01Z");
    expect_true("materialized corruption source stores as reference", source.ok());

    expect_true(
        "hostile materialized state writes without geometry",
        raw_exec(
            fixture.path(),
            "UPDATE aeris_source SET materialization_state=1,"
            "retrieved_at_utc='2026-08-19T08:11:02Z' "
            "WHERE source_id='source.materialized.corrupt';"));

    expect_schema_invalid(
        "deep audit rejects materialized source without geometry",
        project->verify_integrity());
}

}  // namespace

int main() {
    test_referenced_source_with_geometry_is_rejected();
    test_materialized_source_without_geometry_is_rejected();

    if (failures != 0) {
        std::cerr << failures << " source-state corruption assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "project_storage_source_materialization_corruption: PASS\n";
    return EXIT_SUCCESS;
}
