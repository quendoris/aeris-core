// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#include "aeris/storage/dataset.hpp"

#include "geometry_detail.hpp"
#include "provenance_detail.hpp"
#include "sqlite_detail.hpp"

#include <limits>
#include <optional>
#include <utility>

#include <sqlite3.h>

namespace aeris::storage {
namespace {

Status validate_project_connection(sqlite3* db, const ProjectStore& project) {
    Status status = detail::verify_quick_check(db);
    if (!status) return status;

    sqlite3_int64 application_id = 0;
    if (!(status = detail::query_single_int(db, "PRAGMA application_id;", application_id))) return status;
    if (application_id != static_cast<sqlite3_int64>(kProjectApplicationId)) {
        return {StorageError::invalid_application_id,
                "source dataset target is not an AERIS project"};
    }

    sqlite3_int64 generation = 0;
    if (!(status = detail::query_single_int(db, "PRAGMA user_version;", generation))) return status;
    if (generation != kProjectSchemaGeneration) {
        return {StorageError::unsupported_schema,
                "source dataset target has unsupported draft schema generation"};
    }

    std::string uuid;
    if (!(status = detail::query_single_text(
              db, "SELECT project_uuid FROM aeris_meta WHERE id=1;", uuid))) {
        return status;
    }
    if (uuid != project.metadata().project_uuid) {
        return {StorageError::schema_invalid,
                "source dataset project UUID differs from the validated project handle"};
    }
    return Status::success();
}

Status row_exists(
    sqlite3* db,
    const char* sql,
    const std::string& source_id,
    bool& exists) {
    detail::StmtPtr stmt;
    Status status = detail::prepare(db, sql, stmt);
    if (!status) return status;
    if (!(status = detail::bind_text(db, stmt.get(), 1, source_id))) return status;

    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_DONE) {
        exists = false;
        return Status::success();
    }
    if (rc != SQLITE_ROW || sqlite3_column_type(stmt.get(), 0) != SQLITE_INTEGER) {
        return {StorageError::schema_invalid,
                "source dataset existence probe returned malformed data"};
    }
    exists = true;
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        return {StorageError::schema_invalid,
                "source dataset existence probe returned duplicate rows"};
    }
    return Status::success();
}

Status advance_project_revision(
    sqlite3* db,
    const ProjectStore& project,
    const std::string_view modified_utc) {
    sqlite3_int64 current_revision = 0;
    Status status = detail::query_single_int(
        db, "SELECT revision FROM aeris_meta WHERE id=1;", current_revision);
    if (!status) return status;
    if (current_revision < 0 ||
        current_revision == std::numeric_limits<sqlite3_int64>::max()) {
        return {StorageError::schema_invalid,
                "project revision cannot be incremented for source dataset mutation"};
    }

    detail::StmtPtr meta_stmt;
    status = detail::prepare(
        db,
        "UPDATE aeris_meta SET revision=?,modified_utc=? WHERE id=1 AND project_uuid=?;",
        meta_stmt);
    if (status) status = detail::bind_int64(db, meta_stmt.get(), 1, current_revision + 1);
    if (status) status = detail::bind_text(db, meta_stmt.get(), 2, std::string(modified_utc));
    if (status) status = detail::bind_text(db, meta_stmt.get(), 3, project.metadata().project_uuid);
    if (status) status = detail::step_done(db, meta_stmt.get());
    if (status && sqlite3_changes(db) != 1) {
        return {StorageError::schema_invalid,
                "source dataset mutation could not advance exactly one project metadata row"};
    }
    return status;
}

}  // namespace

SourceDatasetMutationResult store_source_dataset(
    ProjectStore& project,
    const SourceDatasetRecord& input,
    const std::string_view modified_utc) {
    if (!is_canonical_utc_timestamp(modified_utc)) {
        return {{StorageError::invalid_argument,
                 "source dataset mutation timestamp is not canonical Gregorian UTC"},
                false, false};
    }

    SourceDatasetRecord record = input;
    Status status = detail::canonicalize_and_validate_source_snapshot_record(record.source);
    if (!status) return {std::move(status), false, false};
    status = detail::canonicalize_and_validate_source_geometry_record(record.geometry);
    if (!status) return {std::move(status), false, false};

    if (record.source.source_id != record.geometry.source_id) {
        return {{StorageError::invalid_argument,
                 "source dataset provenance and geometry source IDs must match"},
                false, false};
    }

    detail::DbPtr db;
    status = detail::open_database(project.path(), SQLITE_OPEN_READWRITE, db);
    if (!status) return {std::move(status), false, false};
    if (!(status = validate_project_connection(db.get(), project))) {
        return {std::move(status), false, false};
    }
    if (!(status = detail::configure_durable(db.get()))) {
        return {std::move(status), false, false};
    }
    if (!(status = detail::begin_immediate(db.get()))) {
        return {std::move(status), false, false};
    }

    bool source_present = false;
    bool geometry_present = false;
    status = row_exists(
        db.get(), "SELECT 1 FROM aeris_source WHERE source_id=?;",
        record.source.source_id, source_present);
    if (status) {
        status = row_exists(
            db.get(), "SELECT 1 FROM aeris_source_geometry WHERE source_id=?;",
            record.source.source_id, geometry_present);
    }
    if (!status) {
        detail::rollback(db.get());
        return {std::move(status), false, false};
    }
    if (!source_present && geometry_present) {
        detail::rollback(db.get());
        return {{StorageError::schema_invalid,
                 "source geometry marker exists without its provenance parent"},
                false, false};
    }

    if (source_present) {
        std::optional<SourceSnapshotRecord> existing_source;
        status = detail::load_source_snapshot_record(
            db.get(), record.source.source_id, existing_source);
        if (!status || !existing_source.has_value()) {
            detail::rollback(db.get());
            if (!status) return {std::move(status), false, false};
            return {{StorageError::schema_invalid,
                     "source existence probe disagrees with canonical provenance reader"},
                    false, false};
        }
        if (!detail::equal_source_snapshot_records(*existing_source, record.source)) {
            detail::rollback(db.get());
            return {{StorageError::record_exists,
                     "source ID already exists with different immutable provenance"},
                    false, false};
        }
    }

    if (geometry_present) {
        std::optional<SourceGeometryRecord> existing_geometry;
        status = detail::load_source_geometry_record(
            db.get(), record.geometry.source_id, existing_geometry);
        detail::rollback(db.get());
        if (!status) return {std::move(status), false, false};
        if (!existing_geometry.has_value()) {
            return {{StorageError::schema_invalid,
                     "geometry existence probe disagrees with canonical geometry reader"},
                    false, false};
        }
        status = project.refresh_metadata();
        if (!status) return {std::move(status), false, false};
        if (detail::equal_source_geometry_records(*existing_geometry, record.geometry)) {
            return {Status::success(), false, false};
        }
        return {{StorageError::record_exists,
                 "source ID already exists with different immutable canonical geometry"},
                false, false};
    }

    if (!source_present) {
        status = detail::insert_source_snapshot_record(db.get(), record.source);
    }
    if (status) {
        status = detail::insert_source_geometry_record(db.get(), record.geometry);
    }
    if (status) {
        status = advance_project_revision(db.get(), project, modified_utc);
    }
    if (!status) {
        detail::rollback(db.get());
        return {std::move(status), false, false};
    }

    status = detail::commit(db.get());
    if (!status) {
        detail::rollback(db.get());
        return {std::move(status), false, false};
    }

    status = project.refresh_metadata();
    if (!status) return {std::move(status), true, true};
    return {Status::success(), true, true};
}

}  // namespace aeris::storage
