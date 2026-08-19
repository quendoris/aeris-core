// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#include "aeris/storage/provenance.hpp"

#include "aeris/util/text.hpp"
#include "provenance_detail.hpp"
#include "sqlite_detail.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <optional>
#include <set>
#include <utility>

#include <sqlite3.h>

namespace aeris::storage {
namespace {

constexpr std::size_t kMaxIdentifierBytes = 255U;
constexpr std::size_t kMaxUriBytes = 4096U;
constexpr std::size_t kMaxRelativePathBytes = 4096U;
constexpr std::size_t kMaxResourcesPerSource = 4096U;

[[nodiscard]] bool bounded_text(
    const std::string& value,
    const std::size_t max_bytes,
    const bool allow_empty = false
) noexcept {
    return (allow_empty || !value.empty()) && value.size() <= max_bytes &&
           value.find('\0') == std::string::npos && util::is_valid_utf8_nul_free(value);
}

[[nodiscard]] bool valid_materialization_state(
    const SourceMaterializationState state
) noexcept {
    return state == SourceMaterializationState::referenced ||
           state == SourceMaterializationState::materialized;
}

[[nodiscard]] bool canonical_relative_path(const std::string& value) noexcept {
    if (!bounded_text(value, kMaxRelativePathBytes) || value.front() == '/' ||
        value.back() == '/' || value.find('\\') != std::string::npos ||
        value.find(':') != std::string::npos) {
        return false;
    }

    std::size_t start = 0U;
    while (start < value.size()) {
        const std::size_t slash = value.find('/', start);
        const std::size_t end = slash == std::string::npos ? value.size() : slash;
        if (end == start) return false;
        const std::string_view segment(value.data() + start, end - start);
        if (segment == "." || segment == "..") return false;
        start = end + 1U;
    }
    return true;
}

[[nodiscard]] bool portable_retrieval_uri(const std::string& value) noexcept {
    if (!bounded_text(value, kMaxUriBytes)) return false;
    const std::size_t colon = value.find(':');
    if (colon == std::string::npos || colon == 0U) return false;
    const unsigned char first = static_cast<unsigned char>(value.front());
    if (!std::isalpha(first)) return false;
    for (std::size_t index = 1U; index < colon; ++index) {
        const unsigned char c = static_cast<unsigned char>(value[index]);
        if (!(std::isalnum(c) || c == '+' || c == '-' || c == '.')) return false;
    }

    std::string scheme = value.substr(0U, colon);
    std::transform(scheme.begin(), scheme.end(), scheme.begin(), [](const unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (scheme == "file") return false;

    for (const char raw : value) {
        const unsigned char c = static_cast<unsigned char>(raw);
        if (c <= 0x20U || c == 0x7fU) return false;
    }
    return true;
}

Status validate_resource(const SourceResourceRecord& resource) {
    if (!bounded_text(resource.logical_name, kMaxIdentifierBytes)) {
        return {StorageError::invalid_argument,
                "source resource logical name is empty, invalid UTF-8/NUL, or exceeds 255 bytes"};
    }
    if (!is_canonical_sha256(resource.sha256)) {
        return {StorageError::invalid_argument,
                "source resource SHA-256 must be 64 lowercase hexadecimal characters"};
    }
    if (resource.size_bytes &&
        *resource.size_bytes > static_cast<std::uint64_t>(std::numeric_limits<sqlite3_int64>::max())) {
        return {StorageError::invalid_argument,
                "source resource byte length exceeds SQLite signed integer range"};
    }

    const bool has_path = !resource.relative_path.empty();
    const bool has_uri = !resource.retrieval_uri.empty();
    if (has_path != has_uri) {
        return {StorageError::invalid_argument,
                "source resource acquisition recipe requires relative_path and retrieval_uri together"};
    }
    if (has_path && !canonical_relative_path(resource.relative_path)) {
        return {StorageError::invalid_argument,
                "source resource relative path is not canonical portable snapshot syntax"};
    }
    if (has_uri && !portable_retrieval_uri(resource.retrieval_uri)) {
        return {StorageError::invalid_argument,
                "source resource retrieval URI is not portable URI syntax"};
    }
    return Status::success();
}

Status validate_record(const SourceSnapshotRecord& record) {
    if (!bounded_text(record.source_id, kMaxIdentifierBytes) ||
        !bounded_text(record.adapter_id, kMaxIdentifierBytes) ||
        !bounded_text(record.provider, kMaxIdentifierBytes) ||
        !bounded_text(record.dataset, kMaxIdentifierBytes) ||
        !bounded_text(record.snapshot, kMaxIdentifierBytes) ||
        !bounded_text(record.dataset_version, kMaxIdentifierBytes) ||
        !bounded_text(record.license_id, kMaxIdentifierBytes) ||
        !bounded_text(record.source_uri, kMaxUriBytes) ||
        !bounded_text(record.worldview, kMaxIdentifierBytes, true)) {
        return {StorageError::invalid_argument,
                "source snapshot text field violates canonical storage bounds/UTF-8"};
    }
    if (record.capability_bits == 0U) {
        return {StorageError::invalid_argument,
                "source snapshot must declare at least one capability bit"};
    }
    if (!is_canonical_sha256(record.content_sha256)) {
        return {StorageError::invalid_argument,
                "source snapshot content SHA-256 must be 64 lowercase hexadecimal characters"};
    }
    if (!valid_materialization_state(record.materialization_state)) {
        return {StorageError::invalid_argument,
                "source materialization state is outside the generation-9 enum domain"};
    }
    if (record.materialization_state == SourceMaterializationState::materialized) {
        if (!is_canonical_utc_timestamp(record.retrieved_at_utc)) {
            return {StorageError::invalid_argument,
                    "materialized source requires canonical verified retrieval UTC"};
        }
    } else if (!record.retrieved_at_utc.empty() &&
               !is_canonical_utc_timestamp(record.retrieved_at_utc)) {
        return {StorageError::invalid_argument,
                "referenced source retrieval UTC must be empty or canonical"};
    }
    if (record.resources.size() > kMaxResourcesPerSource) {
        return {StorageError::invalid_argument,
                "source snapshot exceeds the 4096-resource draft bound"};
    }

    std::set<std::string> logical_names;
    std::set<std::string> relative_paths;
    for (const SourceResourceRecord& resource : record.resources) {
        Status status = validate_resource(resource);
        if (!status) return status;
        if (!logical_names.insert(resource.logical_name).second) {
            return {StorageError::invalid_argument,
                    "source snapshot contains a duplicate logical resource name"};
        }
        if (!resource.relative_path.empty() &&
            !relative_paths.insert(resource.relative_path).second) {
            return {StorageError::invalid_argument,
                    "source snapshot acquisition recipe contains a duplicate relative path"};
        }
    }
    return Status::success();
}

void canonicalize_resources(SourceSnapshotRecord& record) {
    std::sort(record.resources.begin(), record.resources.end(),
              [](const SourceResourceRecord& a, const SourceResourceRecord& b) {
                  return a.logical_name < b.logical_name;
              });
}

[[nodiscard]] bool equal_resource_reference(
    const SourceResourceRecord& a,
    const SourceResourceRecord& b
) noexcept {
    return a.logical_name == b.logical_name && a.sha256 == b.sha256 &&
           a.size_bytes == b.size_bytes && a.relative_path == b.relative_path &&
           a.retrieval_uri == b.retrieval_uri;
}

[[nodiscard]] bool equal_reference_identity(
    const SourceSnapshotRecord& a,
    const SourceSnapshotRecord& b
) noexcept {
    if (a.source_id != b.source_id || a.adapter_id != b.adapter_id ||
        a.capability_bits != b.capability_bits || a.temporal_class != b.temporal_class ||
        a.provider != b.provider || a.dataset != b.dataset || a.snapshot != b.snapshot ||
        a.dataset_version != b.dataset_version || a.source_uri != b.source_uri ||
        a.license_id != b.license_id || a.content_sha256 != b.content_sha256 ||
        a.worldview != b.worldview || a.resources.size() != b.resources.size()) {
        return false;
    }
    for (std::size_t index = 0U; index < a.resources.size(); ++index) {
        if (!equal_resource_reference(a.resources[index], b.resources[index])) return false;
    }
    return true;
}

[[nodiscard]] std::string text_column(sqlite3_stmt* stmt, const int column) {
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
        return {StorageError::invalid_application_id,
                "source catalog target is not an AERIS project"};
    }
    sqlite3_int64 generation = 0;
    if (!(status = detail::query_single_int(db, "PRAGMA user_version;", generation))) return status;
    if (generation != kProjectSchemaGeneration) {
        return {StorageError::unsupported_schema,
                "source catalog target has unsupported draft schema generation"};
    }
    std::string uuid;
    if (!(status = detail::query_single_text(
              db, "SELECT project_uuid FROM aeris_meta WHERE id=1;", uuid))) return status;
    if (uuid != project.metadata().project_uuid) {
        return {StorageError::schema_invalid,
                "source catalog project UUID differs from the validated project handle"};
    }
    return Status::success();
}

Status load_resources(
    sqlite3* db,
    const std::string& source_id,
    std::vector<SourceResourceRecord>& resources
) {
    detail::StmtPtr stmt;
    Status status = detail::prepare(
        db,
        "SELECT logical_name,sha256,size_bytes,relative_path,retrieval_uri "
        "FROM aeris_source_resource WHERE source_id=? ORDER BY logical_name;",
        stmt);
    if (!status) return status;
    if (!(status = detail::bind_text(db, stmt.get(), 1, source_id))) return status;

    while (true) {
        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW) {
            return {StorageError::sqlite_failure,
                    detail::sqlite_message(db, "could not read source resources")};
        }
        if (sqlite3_column_type(stmt.get(), 0) != SQLITE_TEXT ||
            sqlite3_column_type(stmt.get(), 1) != SQLITE_TEXT ||
            (sqlite3_column_type(stmt.get(), 2) != SQLITE_NULL &&
             sqlite3_column_type(stmt.get(), 2) != SQLITE_INTEGER) ||
            sqlite3_column_type(stmt.get(), 3) != SQLITE_TEXT ||
            sqlite3_column_type(stmt.get(), 4) != SQLITE_TEXT) {
            return {StorageError::schema_invalid,
                    "stored source resource contains invalid SQLite types"};
        }

        SourceResourceRecord resource{};
        resource.logical_name = text_column(stmt.get(), 0);
        resource.sha256 = text_column(stmt.get(), 1);
        if (sqlite3_column_type(stmt.get(), 2) == SQLITE_INTEGER) {
            const sqlite3_int64 size = sqlite3_column_int64(stmt.get(), 2);
            if (size < 0) {
                return {StorageError::schema_invalid,
                        "stored source resource has negative byte length"};
            }
            resource.size_bytes = static_cast<std::uint64_t>(size);
        }
        resource.relative_path = text_column(stmt.get(), 3);
        resource.retrieval_uri = text_column(stmt.get(), 4);
        status = validate_resource(resource);
        if (!status) {
            return {StorageError::schema_invalid,
                    "stored source resource violates canonical bounds: " + status.diagnostic};
        }
        resources.push_back(std::move(resource));
        if (resources.size() > kMaxResourcesPerSource) {
            return {StorageError::schema_invalid,
                    "stored source exceeds the 4096-resource draft bound"};
        }
    }
    return Status::success();
}

Status read_source_row(sqlite3* db, sqlite3_stmt* stmt, SourceSnapshotRecord& record) {
    for (int column : {0, 1, 4, 5, 6, 7, 8, 9, 10, 11, 12}) {
        if (sqlite3_column_type(stmt, column) != SQLITE_TEXT) {
            return {StorageError::schema_invalid,
                    "stored source snapshot contains invalid text column type"};
        }
    }
    for (int column : {2, 3, 13}) {
        if (sqlite3_column_type(stmt, column) != SQLITE_INTEGER) {
            return {StorageError::schema_invalid,
                    "stored source snapshot contains invalid integer column type"};
        }
    }

    const sqlite3_int64 capability = sqlite3_column_int64(stmt, 2);
    const sqlite3_int64 temporal = sqlite3_column_int64(stmt, 3);
    const sqlite3_int64 materialization = sqlite3_column_int64(stmt, 13);
    if (capability <= 0 ||
        capability > static_cast<sqlite3_int64>(std::numeric_limits<std::uint32_t>::max()) ||
        temporal < 0 || temporal > static_cast<sqlite3_int64>(std::numeric_limits<std::uint8_t>::max()) ||
        (materialization != 0 && materialization != 1)) {
        return {StorageError::schema_invalid,
                "stored source capability/temporal/materialization value is out of range"};
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
    record.materialization_state = static_cast<SourceMaterializationState>(materialization);
    return load_resources(db, record.source_id, record.resources);
}

Status load_existing(
    sqlite3* db,
    const std::string& source_id,
    std::optional<SourceSnapshotRecord>& record
) {
    detail::StmtPtr stmt;
    Status status = detail::prepare(
        db,
        "SELECT source_id,adapter_id,capability_bits,temporal_class,provider,dataset,snapshot,"
        "dataset_version,source_uri,license_id,content_sha256,retrieved_at_utc,worldview,"
        "materialization_state FROM aeris_source WHERE source_id=?;",
        stmt);
    if (!status) return status;
    if (!(status = detail::bind_text(db, stmt.get(), 1, source_id))) return status;
    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_DONE) return Status::success();
    if (rc != SQLITE_ROW) {
        return {StorageError::sqlite_failure,
                detail::sqlite_message(db, "could not read existing source snapshot")};
    }
    SourceSnapshotRecord loaded{};
    if (!(status = read_source_row(db, stmt.get(), loaded))) return status;
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        return {StorageError::schema_invalid,
                "source ID resolves to more than one row"};
    }
    if (!(status = validate_record(loaded))) {
        return {StorageError::schema_invalid,
                "stored source snapshot violates canonical bounds: " + status.diagnostic};
    }
    canonicalize_resources(loaded);
    record = std::move(loaded);
    return Status::success();
}

Status insert_record(sqlite3* db, const SourceSnapshotRecord& record) {
    detail::StmtPtr source_stmt;
    Status status = detail::prepare(
        db,
        "INSERT INTO aeris_source(source_id,adapter_id,capability_bits,temporal_class,provider,dataset,"
        "snapshot,dataset_version,source_uri,license_id,content_sha256,retrieved_at_utc,worldview,"
        "materialization_state) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?);",
        source_stmt);
    if (!status) return status;

    int index = 1;
    if (!(status = detail::bind_text(db, source_stmt.get(), index++, record.source_id))) return status;
    if (!(status = detail::bind_text(db, source_stmt.get(), index++, record.adapter_id))) return status;
    if (!(status = detail::bind_int64(db, source_stmt.get(), index++, static_cast<sqlite3_int64>(record.capability_bits)))) return status;
    if (!(status = detail::bind_int64(db, source_stmt.get(), index++, static_cast<sqlite3_int64>(record.temporal_class)))) return status;
    if (!(status = detail::bind_text(db, source_stmt.get(), index++, record.provider))) return status;
    if (!(status = detail::bind_text(db, source_stmt.get(), index++, record.dataset))) return status;
    if (!(status = detail::bind_text(db, source_stmt.get(), index++, record.snapshot))) return status;
    if (!(status = detail::bind_text(db, source_stmt.get(), index++, record.dataset_version))) return status;
    if (!(status = detail::bind_text(db, source_stmt.get(), index++, record.source_uri))) return status;
    if (!(status = detail::bind_text(db, source_stmt.get(), index++, record.license_id))) return status;
    if (!(status = detail::bind_text(db, source_stmt.get(), index++, record.content_sha256))) return status;
    if (!(status = detail::bind_text(db, source_stmt.get(), index++, record.retrieved_at_utc))) return status;
    if (!(status = detail::bind_text(db, source_stmt.get(), index++, record.worldview))) return status;
    if (!(status = detail::bind_int64(
              db, source_stmt.get(), index,
              static_cast<sqlite3_int64>(static_cast<std::uint8_t>(record.materialization_state))))) {
        return status;
    }
    if (!(status = detail::step_done(db, source_stmt.get()))) return status;

    for (const SourceResourceRecord& resource : record.resources) {
        detail::StmtPtr resource_stmt;
        status = detail::prepare(
            db,
            "INSERT INTO aeris_source_resource(source_id,logical_name,sha256,size_bytes,relative_path,"
            "retrieval_uri) VALUES(?,?,?,?,?,?);",
            resource_stmt);
        if (!status) return status;
        if (!(status = detail::bind_text(db, resource_stmt.get(), 1, record.source_id))) return status;
        if (!(status = detail::bind_text(db, resource_stmt.get(), 2, resource.logical_name))) return status;
        if (!(status = detail::bind_text(db, resource_stmt.get(), 3, resource.sha256))) return status;
        if (resource.size_bytes) {
            if (!(status = detail::bind_int64(
                      db, resource_stmt.get(), 4,
                      static_cast<sqlite3_int64>(*resource.size_bytes)))) return status;
        } else if (sqlite3_bind_null(resource_stmt.get(), 4) != SQLITE_OK) {
            return {StorageError::sqlite_failure,
                    detail::sqlite_message(db, "sqlite bind NULL failed")};
        }
        if (!(status = detail::bind_text(db, resource_stmt.get(), 5, resource.relative_path))) return status;
        if (!(status = detail::bind_text(db, resource_stmt.get(), 6, resource.retrieval_uri))) return status;
        if (!(status = detail::step_done(db, resource_stmt.get()))) return status;
    }
    return Status::success();
}

Status advance_project_revision(
    sqlite3* db,
    const ProjectStore& project,
    const std::string_view modified_utc
) {
    sqlite3_int64 current_revision = 0;
    Status status = detail::query_single_int(
        db, "SELECT revision FROM aeris_meta WHERE id=1;", current_revision);
    if (!status) return status;
    if (current_revision < 0 ||
        current_revision == std::numeric_limits<sqlite3_int64>::max()) {
        return {StorageError::schema_invalid,
                "project revision cannot be incremented for source reference mutation"};
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
                "source reference mutation could not advance exactly one metadata row"};
    }
    return status;
}

[[nodiscard]] Status row_exists(
    sqlite3* db,
    const char* sql,
    const std::string& source_id,
    bool& exists
) {
    detail::StmtPtr stmt;
    Status status = detail::prepare(db, sql, stmt);
    if (!status) return status;
    if (!(status = detail::bind_text(db, stmt.get(), 1, source_id))) return status;
    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_DONE) {
        exists = false;
        return Status::success();
    }
    if (rc != SQLITE_ROW) {
        return {StorageError::sqlite_failure,
                detail::sqlite_message(db, "source state existence probe failed")};
    }
    exists = true;
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        return {StorageError::schema_invalid,
                "source state existence probe returned duplicate rows"};
    }
    return Status::success();
}

}  // namespace

bool is_canonical_sha256(const std::string_view value) noexcept {
    if (value.size() != 64U) return false;
    for (const char c : value) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

bool source_reference_is_fetchable(const SourceSnapshotRecord& record) noexcept {
    if (record.resources.empty()) return false;
    for (const SourceResourceRecord& resource : record.resources) {
        if (resource.relative_path.empty() || resource.retrieval_uri.empty() ||
            !canonical_relative_path(resource.relative_path) ||
            !portable_retrieval_uri(resource.retrieval_uri)) {
            return false;
        }
    }
    return true;
}

SourceSnapshotMutationResult store_source_snapshot(
    ProjectStore& project,
    const SourceSnapshotRecord& input,
    const std::string_view modified_utc
) {
    if (!is_canonical_utc_timestamp(modified_utc)) {
        return {{StorageError::invalid_argument,
                 "source reference mutation timestamp is not canonical Gregorian UTC"},
                false, false};
    }

    SourceSnapshotRecord record = input;
    record.materialization_state = SourceMaterializationState::referenced;
    Status status = validate_record(record);
    if (!status) return {std::move(status), false, false};
    canonicalize_resources(record);

    detail::DbPtr db;
    status = detail::open_database(project.path(), SQLITE_OPEN_READWRITE, db);
    if (!status) return {std::move(status), false, false};
    if (!(status = validate_project_connection(db.get(), project)) ||
        !(status = detail::configure_durable(db.get())) ||
        !(status = detail::begin_immediate(db.get()))) {
        return {std::move(status), false, false};
    }

    std::optional<SourceSnapshotRecord> existing;
    status = load_existing(db.get(), record.source_id, existing);
    if (!status) {
        detail::rollback(db.get());
        return {std::move(status), false, false};
    }
    if (existing.has_value()) {
        detail::rollback(db.get());
        status = project.refresh_metadata();
        if (!status) return {std::move(status), false, false};
        if (equal_reference_identity(*existing, record)) {
            return {Status::success(), false, false};
        }
        return {{StorageError::record_exists,
                 "source ID already exists with different immutable identity/acquisition recipe"},
                false, false};
    }

    if (!(status = insert_record(db.get(), record)) ||
        !(status = advance_project_revision(db.get(), project, modified_utc))) {
        detail::rollback(db.get());
        return {std::move(status), false, false};
    }
    if (!(status = detail::commit(db.get()))) {
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
    if (!(status = validate_project_connection(db.get(), project))) {
        return {std::move(status), {}};
    }

    detail::StmtPtr stmt;
    status = detail::prepare(
        db.get(),
        "SELECT source_id,adapter_id,capability_bits,temporal_class,provider,dataset,snapshot,"
        "dataset_version,source_uri,license_id,content_sha256,retrieved_at_utc,worldview,"
        "materialization_state FROM aeris_source ORDER BY source_id;",
        stmt);
    if (!status) return {std::move(status), {}};

    SourceSnapshotListResult result{};
    while (true) {
        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW) {
            return {{StorageError::sqlite_failure,
                     detail::sqlite_message(db.get(), "could not enumerate source catalog")}, {}};
        }
        SourceSnapshotRecord record{};
        status = read_source_row(db.get(), stmt.get(), record);
        if (!status) return {std::move(status), {}};
        if (!(status = validate_record(record))) {
            return {{StorageError::schema_invalid,
                     "stored source snapshot violates canonical bounds: " + status.diagnostic}, {}};
        }
        canonicalize_resources(record);
        result.records.push_back(std::move(record));
    }
    result.status = Status::success();
    return result;
}

namespace detail {

Status verify_source_semantics(const ProjectStore& project) {
    const SourceSnapshotListResult sources = list_source_snapshots(project);
    if (!sources.ok()) return sources.status;

    DbPtr db;
    Status status = open_database(project.path(), SQLITE_OPEN_READONLY, db);
    if (!status) return status;
    if (!(status = validate_project_connection(db.get(), project))) return status;

    for (const SourceSnapshotRecord& source : sources.records) {
        bool geometry = false;
        bool properties = false;
        if (!(status = row_exists(
                  db.get(), "SELECT 1 FROM aeris_source_geometry WHERE source_id=?;",
                  source.source_id, geometry)) ||
            !(status = row_exists(
                  db.get(), "SELECT 1 FROM aeris_source_feature_properties WHERE source_id=?;",
                  source.source_id, properties))) {
            return status;
        }

        if (source.materialization_state == SourceMaterializationState::referenced) {
            if (geometry || properties) {
                return {StorageError::schema_invalid,
                        "referenced source owns canonical decoded channel rows: " + source.source_id};
            }
        } else {
            if (!geometry) {
                return {StorageError::schema_invalid,
                        "materialized generation-9 vector source lacks canonical geometry: " +
                            source.source_id};
            }
            if (!is_canonical_utc_timestamp(source.retrieved_at_utc)) {
                return {StorageError::schema_invalid,
                        "materialized source lacks canonical verified retrieval UTC: " +
                            source.source_id};
            }
        }
        if (properties && !geometry) {
            return {StorageError::schema_invalid,
                    "source feature properties exist without canonical geometry: " + source.source_id};
        }
    }
    return Status::success();
}

}  // namespace detail
}  // namespace aeris::storage
