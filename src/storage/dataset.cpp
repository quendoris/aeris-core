// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#include "aeris/storage/dataset.hpp"

#include "geometry_detail.hpp"
#include "provenance_detail.hpp"
#include "sqlite_detail.hpp"

#include <limits>
#include <string>
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
        return {StorageError::invalid_application_id, "dataset target is not an AERIS project"};
    }

    sqlite3_int64 generation = 0;
    if (!(status = detail::query_single_int(db, "PRAGMA user_version;", generation))) return status;
    if (generation != kProjectSchemaGeneration) {
        return {StorageError::unsupported_schema, "dataset target has unsupported draft schema generation"};
    }

    std::string uuid;
    if (!(status = detail::query_single_text(db, "SELECT project_uuid FROM aeris_meta WHERE id=1;", uuid))) return status;
    if (uuid != project.metadata().project_uuid) {
        return {StorageError::schema_invalid, "dataset target UUID differs from the validated project handle"};
    }
    return Status::success();
}

Status advance_project_revision(
    sqlite3* db,
    const ProjectStore& project,
    const std::string_view modified_utc) {
    sqlite3_int64 current_revision = 0;
    Status status = detail::query_single_int(db, "SELECT revision FROM aeris_meta WHERE id=1;", current_revision);
    if (!status) return status;
    if (current_revision < 0 || current_revision == std::numeric_limits<sqlite3_int64>::max()) {
        return {StorageError::schema_invalid, "project revision cannot be incremented for dataset mutation"};
    }

    detail::StmtPtr stmt;
    status = detail::prepare(
        db,
        "UPDATE aeris_meta SET revision=?,modified_utc=? WHERE id=1 AND project_uuid=?;",
        stmt);
    if (status) status = detail::bind_int64(db, stmt.get(), 1, current_revision + 1);
    if (status) status = detail::bind_text(db, stmt.get(), 2, std::string(modified_utc));
    if (status) status = detail::bind_text(db, stmt.get(), 3, project.metadata().project_uuid);
    if (status) status = detail::step_done(db, stmt.get());
    if (status && sqlite3_changes(db) != 1) {
        return {StorageError::schema_invalid,
                "dataset mutation could not advance exactly one project metadata row"};
    }
    return status;
}

SourceDatasetMutationResult fail_transaction(
    sqlite3* db,
    Status status,
    const bool refresh,
    ProjectStore& project) {
    detail::rollback(db);
    if (refresh) {
        Status refreshed = project.refresh_metadata();
        if (!refreshed) return {std::move(refreshed), false, false};
    }
    return {std::move(status), false, false};
}

}  // namespace

SourceDatasetMutationResult store_source_dataset(
    ProjectStore& project,
    const SourceDatasetRecord& input,
    const std::string_view modified_utc) {
    if (!is_canonical_utc_timestamp(modified_utc)) {
        return {{StorageError::invalid_argument,
                 "source dataset mutation timestamp is not canonical Gregorian UTC"},
                false,
                false};
    }
    if (input.provenance.source_id != input.geometry.source_id) {
        return {{StorageError::invalid_argument,
                 "source dataset provenance and geometry must use the same project source ID"},
                false,
                false};
    }

    SourceDatasetRecord record = input;
    Status status = detail::prepare_source_snapshot(record.provenance);
    if (!status) return {std::move(status), false, false};
    status = detail::prepare_source_geometry(record.geometry);
    if (!status) return {std::move(status), false, false};

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

    detail::ExistingRecordState provenance_state = detail::ExistingRecordState::absent;
    status = detail::inspect_source_snapshot(db.get(), record.provenance, provenance_state);
    if (!status) return fail_transaction(db.get(), std::move(status), false, project);

    detail::ExistingRecordState geometry_state = detail::ExistingRecordState::absent;
    status = detail::inspect_source_geometry(db.get(), record.geometry, geometry_state);
    if (!status) return fail_transaction(db.get(), std::move(status), false, project);

    if (provenance_state == detail::ExistingRecordState::identical &&
        geometry_state == detail::ExistingRecordState::identical) {
        detail::rollback(db.get());
        status = project.refresh_metadata();
        if (!status) return {std::move(status), false, false};
        return {Status::success(), false, false};
    }

    if (provenance_state != detail::ExistingRecordState::absent ||
        geometry_state != detail::ExistingRecordState::absent) {
        const bool partial =
            (provenance_state == detail::ExistingRecordState::identical &&
             geometry_state == detail::ExistingRecordState::absent) ||
            (provenance_state == detail::ExistingRecordState::absent &&
             geometry_state == detail::ExistingRecordState::identical);
        const std::string diagnostic = partial
            ? "source dataset has pre-existing partial low-level state and cannot be reclassified as an atomic ingestion"
            : "source dataset ID already exists with conflicting immutable provenance or geometry";
        return fail_transaction(
            db.get(),
            {StorageError::record_exists, diagnostic},
            true,
            project);
    }

    status = detail::insert_source_snapshot_rows(db.get(), record.provenance);
    if (!status) return fail_transaction(db.get(), std::move(status), false, project);

    status = detail::insert_source_geometry_rows(db.get(), record.geometry);
    if (!status) return fail_transaction(db.get(), std::move(status), false, project);

    status = advance_project_revision(db.get(), project, modified_utc);
    if (!status) return fail_transaction(db.get(), std::move(status), false, project);

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
