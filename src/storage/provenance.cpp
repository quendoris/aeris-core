// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#include "aeris/storage/provenance.hpp"

#include "aeris/storage/geography.hpp"
#include "geography_codec.hpp"
#include "sqlite_detail.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <utility>

#include <sqlite3.h>

namespace aeris::storage {
namespace {

constexpr std::size_t kMaxIdentifierBytes = 255U;
constexpr std::size_t kMaxUriBytes = 4096U;
constexpr std::size_t kMaxResourcesPerSource = 4096U;

bool bounded_text(const std::string& value, const std::size_t max_bytes, const bool allow_empty = false) noexcept {
    return (allow_empty || !value.empty()) && value.size() <= max_bytes && value.find('\0') == std::string::npos;
}

Status validate_resource(const SourceResourceRecord& resource) {
    if (!bounded_text(resource.logical_name, kMaxIdentifierBytes)) {
        return {StorageError::invalid_argument, "source resource logical name is empty, contains NUL, or exceeds 255 bytes"};
    }
    if (!is_canonical_sha256(resource.sha256)) {
        return {StorageError::invalid_argument, "source resource SHA-256 must be 64 lowercase hexadecimal characters"};
    }
    if (resource.size_bytes && *resource.size_bytes > static_cast<std::uint64_t>(std::numeric_limits<sqlite3_int64>::max())) {
        return {StorageError::invalid_argument, "source resource byte length exceeds SQLite signed integer range"};
    }
    return Status::success();
}

Status validate_record(const SourceSnapshotRecord& record) {
    if (!bounded_text(record.source_id, kMaxIdentifierBytes) || !bounded_text(record.adapter_id, kMaxIdentifierBytes) ||
        !bounded_text(record.provider, kMaxIdentifierBytes) || !bounded_text(record.dataset, kMaxIdentifierBytes) ||
        !bounded_text(record.snapshot, kMaxIdentifierBytes) || !bounded_text(record.dataset_version, kMaxIdentifierBytes) ||
        !bounded_text(record.license_id, kMaxIdentifierBytes) || !bounded_text(record.source_uri, kMaxUriBytes) ||
        !bounded_text(record.worldview, kMaxIdentifierBytes, true)) {
        return {StorageError::invalid_argument, "source snapshot text field violates canonical storage bounds"};
    }
    if (record.capability_bits == 0U) {
        return {StorageError::invalid_argument, "source snapshot must declare at least one capability bit"};
    }
    if (!is_canonical_sha256(record.content_sha256)) {
        return {StorageError::invalid_argument, "source snapshot content SHA-256 must be 64 lowercase hexadecimal characters"};
    }
    if (!is_canonical_utc_timestamp(record.retrieved_at_utc)) {
        return {StorageError::invalid_argument, "source snapshot retrieval timestamp is not canonical Gregorian UTC"};
    }
    if (record.resources.size() > kMaxResourcesPerSource) {
        return {StorageError::invalid_argument, "source snapshot exceeds the 4096-resource draft bound"};
    }
    std::set<std::string> logical_names;
    for (const SourceResourceRecord& resource : record.resources) {
        Status status = validate_resource(resource);
        if (!status) return status;
        if (!logical_names.insert(resource.logical_name).second) {
            return {StorageError::invalid_argument, "source snapshot contains a duplicate logical resource name"};
        }
    }
    return Status::success();
}

void canonicalize_resources(SourceSnapshotRecord& record) {
    std::sort(record.resources.begin(), record.resources.end(), [](const SourceResourceRecord& a, const SourceResourceRecord& b) {
        return a.logical_name < b.logical_name;
    });
}

bool equal_resource(const SourceResourceRecord& a, const SourceResourceRecord& b) {
    return a.logical_name == b.logical_name && a.sha256 == b.sha256 && a.size_bytes == b.size_bytes;
}

bool equal_record(const SourceSnapshotRecord& a, const SourceSnapshotRecord& b) {
    if (a.source_id != b.source_id || a.adapter_id != b.adapter_id || a.capability_bits != b.capability_bits ||
        a.temporal_class != b.temporal_class || a.provider != b.provider || a.dataset != b.dataset ||
        a.snapshot != b.snapshot || a.dataset_version != b.dataset_version || a.source_uri != b.source_uri ||
        a.license_id != b.license_id || a.content_sha256 != b.content_sha256 ||
        a.retrieved_at_utc != b.retrieved_at_utc || a.worldview != b.worldview ||
        a.resources.size() != b.resources.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.resources.size(); ++i) {
        if (!equal_resource(a.resources[i], b.resources[i])) return false;
    }
    return true;
}

std::string text_column(sqlite3_stmt* stmt, const int column) {
    const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, column));
    const int bytes = sqlite3_column_bytes(stmt, column);
    return std::string(text, static_cast<std::size_t>(bytes));
}

Status validate_project_connection(sqlite3* db, const ProjectStore& project) {
    Status status = detail::verify_quick_check(db);
    if (!status) return status;
    sqlite3_int64 application_id = 0;
    if (!(status = detail::query_single_int(db, "PRAGMA application_id;", application_id))) return status;
    if (application_id != static_cast<sqlite3_int64>(kProjectApplicationId)) {
        return {StorageError::invalid_application_id, "source catalog target is not an AERIS project"};
    }
    sqlite3_int64 generation = 0;
    if (!(status = detail::query_single_int(db, "PRAGMA user_version;", generation))) return status;
    if (generation != kProjectSchemaGeneration) {
        return {StorageError::unsupported_schema, "source catalog target has unsupported draft schema generation"};
    }
    std::string uuid;
    if (!(status = detail::query_single_text(db, "SELECT project_uuid FROM aeris_meta WHERE id=1;", uuid))) return status;
    if (uuid != project.metadata().project_uuid) {
        return {StorageError::schema_invalid, "source catalog project UUID differs from the validated project handle"};
    }
    return Status::success();
}

Status load_resources(sqlite3* db, const std::string& source_id, std::vector<SourceResourceRecord>& resources) {
    detail::StmtPtr stmt;
    Status status = detail::prepare(
        db,
        "SELECT logical_name,sha256,size_bytes FROM aeris_source_resource WHERE source_id=? ORDER BY logical_name;",
        stmt);
    if (!status) return status;
    if (!(status = detail::bind_text(db, stmt.get(), 1, source_id))) return status;

    while (true) {
        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW) return {StorageError::sqlite_failure, detail::sqlite_message(db, "could not read source resources")};
        if (sqlite3_column_type(stmt.get(), 0) != SQLITE_TEXT || sqlite3_column_type(stmt.get(), 1) != SQLITE_TEXT ||
            (sqlite3_column_type(stmt.get(), 2) != SQLITE_NULL && sqlite3_column_type(stmt.get(), 2) != SQLITE_INTEGER)) {
            return {StorageError::schema_invalid, "stored source resource contains invalid SQLite types"};
        }
        SourceResourceRecord resource;
        resource.logical_name = text_column(stmt.get(), 0);
        resource.sha256 = text_column(stmt.get(), 1);
        if (sqlite3_column_type(stmt.get(), 2) == SQLITE_INTEGER) {
            const sqlite3_int64 size = sqlite3_column_int64(stmt.get(), 2);
            if (size < 0) return {StorageError::schema_invalid, "stored source resource has negative byte length"};
            resource.size_bytes = static_cast<std::uint64_t>(size);
        }
        status = validate_resource(resource);
        if (!status) return {StorageError::schema_invalid, "stored source resource violates canonical bounds: " + status.diagnostic};
        resources.push_back(std::move(resource));
        if (resources.size() > kMaxResourcesPerSource) {
            return {StorageError::schema_invalid, "stored source exceeds the 4096-resource draft bound"};
        }
    }
    return Status::success();
}

Status read_source_row(sqlite3* db, sqlite3_stmt* stmt, SourceSnapshotRecord& record) {
    for (int column : {0, 1, 4, 5, 6, 7, 8, 9, 10, 11, 12}) {
        if (sqlite3_column_type(stmt, column) != SQLITE_TEXT) {
            return {StorageError::schema_invalid, "stored source snapshot contains invalid text column type"};
        }
    }
    for (int column : {2, 3}) {
        if (sqlite3_column_type(stmt, column) != SQLITE_INTEGER) {
            return {StorageError::schema_invalid, "stored source snapshot contains invalid integer column type"};
        }
    }
    const sqlite3_int64 capability = sqlite3_column_int64(stmt, 2);
    const sqlite3_int64 temporal = sqlite3_column_int64(stmt, 3);
    if (capability <= 0 || capability > static_cast<sqlite3_int64>(std::numeric_limits<std::uint32_t>::max()) ||
        temporal < 0 || temporal > static_cast<sqlite3_int64>(std::numeric_limits<std::uint8_t>::max())) {
        return {StorageError::schema_invalid, "stored source capability or temporal value is out of range"};
    }
    record.source_id = text_column(stmt, 0);
    record.adapter_id = text_column(stmt, 1);
    record.capability_bits = static_cast<std::uint32_t>(capability);
    record.temporal_class = static_cast<std::uint8_t>(temporal);
    record.provider = text_column(stmt, 4);
    record.dataset = text_column(stmt, 5);
    record.snapshot = text_column(stmt, 6);
    record.dataset_version = text_column(stmt, 7);
    record.source_uri = text_column(stmt, 8);
    record.license_id = text_column(stmt, 9);
    record.content_sha256 = text_column(stmt, 10);
    record.retrieved_at_utc = text_column(stmt, 11);
    record.worldview = text_column(stmt, 12);
    return load_resources(db, record.source_id, record.resources);
}

Status load_existing(sqlite3* db, const std::string& source_id, std::optional<SourceSnapshotRecord>& record) {
    detail::StmtPtr stmt;
    Status status = detail::prepare(
        db,
        "SELECT source_id,adapter_id,capability_bits,temporal_class,provider,dataset,snapshot,dataset_version,source_uri,license_id,content_sha256,retrieved_at_utc,worldview FROM aeris_source WHERE source_id=?;",
        stmt);
    if (!status) return status;
    if (!(status = detail::bind_text(db, stmt.get(), 1, source_id))) return status;
    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_DONE) return Status::success();
    if (rc != SQLITE_ROW) return {StorageError::sqlite_failure, detail::sqlite_message(db, "could not read existing source snapshot")};
    SourceSnapshotRecord loaded;
    if (!(status = read_source_row(db, stmt.get(), loaded))) return status;
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) return {StorageError::schema_invalid, "source ID resolves to more than one row"};
    if (!(status = validate_record(loaded))) return {StorageError::schema_invalid, "stored source snapshot violates canonical bounds: " + status.diagnostic};
    canonicalize_resources(loaded);
    record = std::move(loaded);
    return Status::success();
}

Status insert_record(sqlite3* db, const SourceSnapshotRecord& record) {
    detail::StmtPtr source_stmt;
    Status status = detail::prepare(
        db,
        "INSERT INTO aeris_source(source_id,adapter_id,capability_bits,temporal_class,provider,dataset,snapshot,dataset_version,source_uri,license_id,content_sha256,retrieved_at_utc,worldview) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?);",
        source_stmt);
    if (!status) return status;
    int i = 1;
    if (!(status = detail::bind_text(db, source_stmt.get(), i++, record.source_id))) return status;
    if (!(status = detail::bind_text(db, source_stmt.get(), i++, record.adapter_id))) return status;
    if (!(status = detail::bind_int64(db, source_stmt.get(), i++, static_cast<sqlite3_int64>(record.capability_bits)))) return status;
    if (!(status = detail::bind_int64(db, source_stmt.get(), i++, static_cast<sqlite3_int64>(record.temporal_class)))) return status;
    if (!(status = detail::bind_text(db, source_stmt.get(), i++, record.provider))) return status;
    if (!(status = detail::bind_text(db, source_stmt.get(), i++, record.dataset))) return status;
    if (!(status = detail::bind_text(db, source_stmt.get(), i++, record.snapshot))) return status;
    if (!(status = detail::bind_text(db, source_stmt.get(), i++, record.dataset_version))) return status;
    if (!(status = detail::bind_text(db, source_stmt.get(), i++, record.source_uri))) return status;
    if (!(status = detail::bind_text(db, source_stmt.get(), i++, record.license_id))) return status;
    if (!(status = detail::bind_text(db, source_stmt.get(), i++, record.content_sha256))) return status;
    if (!(status = detail::bind_text(db, source_stmt.get(), i++, record.retrieved_at_utc))) return status;
    if (!(status = detail::bind_text(db, source_stmt.get(), i, record.worldview))) return status;
    if (!(status = detail::step_done(db, source_stmt.get()))) return status;

    for (const SourceResourceRecord& resource : record.resources) {
        detail::StmtPtr resource_stmt;
        status = detail::prepare(
            db,
            "INSERT INTO aeris_source_resource(source_id,logical_name,sha256,size_bytes) VALUES(?,?,?,?);",
            resource_stmt);
        if (!status) return status;
        if (!(status = detail::bind_text(db, resource_stmt.get(), 1, record.source_id))) return status;
        if (!(status = detail::bind_text(db, resource_stmt.get(), 2, resource.logical_name))) return status;
        if (!(status = detail::bind_text(db, resource_stmt.get(), 3, resource.sha256))) return status;
        if (resource.size_bytes) {
            if (!(status = detail::bind_int64(db, resource_stmt.get(), 4, static_cast<sqlite3_int64>(*resource.size_bytes)))) return status;
        } else if (sqlite3_bind_null(resource_stmt.get(), 4) != SQLITE_OK) {
            return {StorageError::sqlite_failure, detail::sqlite_message(db, "sqlite bind NULL failed")};
        }
        if (!(status = detail::step_done(db, resource_stmt.get()))) return status;
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
        return {StorageError::schema_invalid, "project revision cannot be incremented for source mutation"};
    }

    detail::StmtPtr meta_stmt;
    status = detail::prepare(db, "UPDATE aeris_meta SET revision=?,modified_utc=? WHERE id=1 AND project_uuid=?;", meta_stmt);
    if (status) status = detail::bind_int64(db, meta_stmt.get(), 1, current_revision + 1);
    if (status) status = detail::bind_text(db, meta_stmt.get(), 2, std::string(modified_utc));
    if (status) status = detail::bind_text(db, meta_stmt.get(), 3, project.metadata().project_uuid);
    if (status) status = detail::step_done(db, meta_stmt.get());
    if (status && sqlite3_changes(db) != 1) {
        return {StorageError::schema_invalid, "source mutation could not advance exactly one project metadata row"};
    }
    return status;
}

}  // namespace

bool is_canonical_sha256(const std::string_view value) noexcept {
    if (value.size() != 64U) return false;
    for (const char c : value) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

SourceSnapshotMutationResult store_source_snapshot(
    ProjectStore& project,
    const SourceSnapshotRecord& input,
    const std::string_view modified_utc) {
    if (!is_canonical_utc_timestamp(modified_utc)) {
        return {{StorageError::invalid_argument, "source snapshot mutation timestamp is not canonical Gregorian UTC"}, false, false};
    }
    SourceSnapshotRecord record = input;
    Status status = validate_record(record);
    if (!status) return {std::move(status), false, false};
    canonicalize_resources(record);

    detail::DbPtr db;
    status = detail::open_database(project.path(), SQLITE_OPEN_READWRITE, db);
    if (!status) return {std::move(status), false, false};
    if (!(status = validate_project_connection(db.get(), project))) return {std::move(status), false, false};
    if (!(status = detail::configure_durable(db.get()))) return {std::move(status), false, false};
    if (!(status = detail::begin_immediate(db.get()))) return {std::move(status), false, false};

    std::optional<SourceSnapshotRecord> existing;
    status = load_existing(db.get(), record.source_id, existing);
    if (!status) {
        detail::rollback(db.get());
        return {std::move(status), false, false};
    }
    if (existing) {
        const bool identical = equal_record(*existing, record);
        detail::rollback(db.get());
        status = project.refresh_metadata();
        if (!status) return {std::move(status), false, false};
        if (identical) return {Status::success(), false, false};
        return {{StorageError::record_exists, "source ID already exists with different immutable provenance"}, false, false};
    }

    status = insert_record(db.get(), record);
    if (!status || !(status = advance_project_revision(db.get(), project, modified_utc))) {
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

SourceDatasetMutationResult store_source_dataset(
    ProjectStore& project,
    const SourceDatasetRecord& input,
    const std::string_view modified_utc) {
    if (!is_canonical_utc_timestamp(modified_utc)) {
        return {{StorageError::invalid_argument, "source dataset mutation timestamp is not canonical Gregorian UTC"}, false, false};
    }

    SourceDatasetRecord record = input;
    Status status = validate_record(record.source);
    if (!status) return {std::move(status), false, false};
    canonicalize_resources(record.source);
    if (!(status = detail::canonicalize_feature_records(record.features))) {
        return {std::move(status), false, false};
    }

    detail::DbPtr db;
    status = detail::open_database(project.path(), SQLITE_OPEN_READWRITE, db);
    if (!status) return {std::move(status), false, false};
    if (!(status = validate_project_connection(db.get(), project))) return {std::move(status), false, false};
    if (!(status = detail::configure_durable(db.get()))) return {std::move(status), false, false};
    if (!(status = detail::begin_immediate(db.get()))) return {std::move(status), false, false};

    std::optional<SourceSnapshotRecord> existing;
    status = load_existing(db.get(), record.source.source_id, existing);
    if (!status) {
        detail::rollback(db.get());
        return {std::move(status), false, false};
    }
    if (existing) {
        std::vector<SourceFeatureRecord> existing_features;
        status = detail::load_feature_records(db.get(), record.source.source_id, existing_features);
        if (!status) {
            detail::rollback(db.get());
            return {std::move(status), false, false};
        }
        const bool identical =
            equal_record(*existing, record.source) &&
            detail::equal_feature_records(existing_features, record.features);
        detail::rollback(db.get());
        status = project.refresh_metadata();
        if (!status) return {std::move(status), false, false};
        if (identical) return {Status::success(), false, false};
        return {{StorageError::record_exists, "source ID already exists with different immutable provenance or geography"}, false, false};
    }

    status = insert_record(db.get(), record.source);
    if (status) status = detail::insert_feature_records(db.get(), record.source.source_id, record.features);
    if (status) status = advance_project_revision(db.get(), project, modified_utc);
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

SourceSnapshotListResult list_source_snapshots(const ProjectStore& project) {
    detail::DbPtr db;
    Status status = detail::open_database(project.path(), SQLITE_OPEN_READONLY, db);
    if (!status) return {std::move(status), {}};
    if (!(status = validate_project_connection(db.get(), project))) return {std::move(status), {}};

    detail::StmtPtr stmt;
    status = detail::prepare(
        db.get(),
        "SELECT source_id,adapter_id,capability_bits,temporal_class,provider,dataset,snapshot,dataset_version,source_uri,license_id,content_sha256,retrieved_at_utc,worldview FROM aeris_source ORDER BY source_id;",
        stmt);
    if (!status) return {std::move(status), {}};

    SourceSnapshotListResult result;
    while (true) {
        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW) {
            result.status = {StorageError::sqlite_failure, detail::sqlite_message(db.get(), "could not enumerate source snapshots")};
            result.records.clear();
            return result;
        }
        SourceSnapshotRecord record;
        status = read_source_row(db.get(), stmt.get(), record);
        if (!status) {
            result.status = std::move(status);
            result.records.clear();
            return result;
        }
        status = validate_record(record);
        if (!status) {
            result.status = {StorageError::schema_invalid, "stored source snapshot violates canonical bounds: " + status.diagnostic};
            result.records.clear();
            return result;
        }
        canonicalize_resources(record);
        result.records.push_back(std::move(record));
    }
    result.status = Status::success();
    return result;
}

SourceFeatureListResult list_source_features(
    const ProjectStore& project,
    const std::string_view source_id) {
    const std::string id(source_id);
    if (!bounded_text(id, kMaxIdentifierBytes)) {
        return {{StorageError::invalid_argument, "source ID is empty, contains NUL, or exceeds 255 bytes"}, {}};
    }

    detail::DbPtr db;
    Status status = detail::open_database(project.path(), SQLITE_OPEN_READONLY, db);
    if (!status) return {std::move(status), {}};
    if (!(status = validate_project_connection(db.get(), project))) return {std::move(status), {}};

    std::optional<SourceSnapshotRecord> existing;
    if (!(status = load_existing(db.get(), id, existing))) return {std::move(status), {}};
    if (!existing) {
        return {{StorageError::invalid_argument, "source ID does not exist in the project"}, {}};
    }

    SourceFeatureListResult result;
    status = detail::load_feature_records(db.get(), id, result.records);
    result.status = std::move(status);
    if (!result.status) result.records.clear();
    return result;
}

}  // namespace aeris::storage