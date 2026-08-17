// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#include "aeris/storage/resource.hpp"

#include "aeris/storage/provenance.hpp"
#include "aeris/util/sha256.hpp"
#include "resource_detail.hpp"
#include "sqlite_detail.hpp"

#include <algorithm>
#include <cctype>
#include <climits>
#include <fstream>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include <sqlite3.h>

namespace aeris::storage {
namespace {

constexpr std::size_t kMaxIdentifierBytes = 255U;
constexpr std::size_t kMaxMediaTypeBytes = 255U;
constexpr std::size_t kMaxRetrievalUriBytes = 4096U;
constexpr std::uint64_t kMaxEmbeddedResourceBytes =
    static_cast<std::uint64_t>(kEmbeddedResourceChunkBytes) *
    static_cast<std::uint64_t>(kMaxEmbeddedResourceChunks);

[[nodiscard]] bool bounded_text(
    const std::string& value,
    const std::size_t max_bytes,
    const bool allow_empty = false) noexcept {
    return (allow_empty || !value.empty()) && value.size() <= max_bytes &&
           value.find('\0') == std::string::npos;
}

[[nodiscard]] bool valid_media_type(const std::string& value) noexcept {
    if (!bounded_text(value, kMaxMediaTypeBytes)) return false;
    for (const char raw : value) {
        const unsigned char c = static_cast<unsigned char>(raw);
        if (c < 0x20U || c == 0x7fU) return false;
    }
    return true;
}

[[nodiscard]] char ascii_lower(const char c) noexcept {
    if (c >= 'A' && c <= 'Z') return static_cast<char>(c - 'A' + 'a');
    return c;
}

[[nodiscard]] bool valid_retrieval_uri(const std::string& value) noexcept {
    if (!bounded_text(value, kMaxRetrievalUriBytes, true)) return false;
    if (value.empty()) return true;

    const std::size_t colon = value.find(':');
    if (colon < 2U) return false;
    if (!std::isalpha(static_cast<unsigned char>(value.front()))) return false;
    for (std::size_t index = 1U; index < colon; ++index) {
        const unsigned char c = static_cast<unsigned char>(value[index]);
        if (!(std::isalnum(c) || c == '+' || c == '-' || c == '.')) return false;
    }

    std::string scheme = value.substr(0U, colon);
    std::transform(scheme.begin(), scheme.end(), scheme.begin(), ascii_lower);
    return scheme != "file";
}

[[nodiscard]] Status validate_identity(const ProjectResourceIdentity& resource) {
    if (!bounded_text(resource.resource_id, kMaxIdentifierBytes)) {
        return {StorageError::invalid_argument,
                "project resource ID is empty, contains NUL, or exceeds 255 bytes"};
    }
    if (!is_canonical_sha256(resource.sha256)) {
        return {StorageError::invalid_argument,
                "project resource SHA-256 must be 64 lowercase hexadecimal characters"};
    }
    if (!valid_media_type(resource.media_type)) {
        return {StorageError::invalid_argument,
                "project resource media type is empty, contains control/NUL data, or exceeds 255 bytes"};
    }
    if (!valid_retrieval_uri(resource.retrieval_uri)) {
        return {StorageError::invalid_argument,
                "project resource retrieval URI must be empty or a portable non-file URI"};
    }
    if (resource.size_bytes > kMaxEmbeddedResourceBytes ||
        resource.size_bytes > static_cast<std::uint64_t>(std::numeric_limits<sqlite3_int64>::max())) {
        return {StorageError::invalid_argument,
                "project resource exceeds the current streaming/chunk-count draft bound"};
    }
    return Status::success();
}

[[nodiscard]] std::uint64_t expected_chunk_count(const std::uint64_t size_bytes) noexcept {
    if (size_bytes == 0U) return 0U;
    const std::uint64_t chunk = static_cast<std::uint64_t>(kEmbeddedResourceChunkBytes);
    return 1U + (size_bytes - 1U) / chunk;
}

// Content identity is immutable. required_for_reproduction is deliberately not
// part of this equality: bindings may monotonically promote an optional resource
// to required without changing the content-addressed object itself.
[[nodiscard]] bool equal_content_identity(
    const ProjectResourceIdentity& a,
    const ProjectResourceIdentity& b) noexcept {
    return a.resource_id == b.resource_id && a.sha256 == b.sha256 &&
           a.media_type == b.media_type && a.size_bytes == b.size_bytes &&
           a.retrieval_uri == b.retrieval_uri;
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
                "resource target is not an AERIS project"};
    }

    sqlite3_int64 generation = 0;
    if (!(status = detail::query_single_int(db, "PRAGMA user_version;", generation))) return status;
    if (generation != kProjectSchemaGeneration) {
        return {StorageError::unsupported_schema,
                "resource target has unsupported draft schema generation"};
    }

    std::string uuid;
    if (!(status = detail::query_single_text(
              db, "SELECT project_uuid FROM aeris_meta WHERE id=1;", uuid))) {
        return status;
    }
    if (uuid != project.metadata().project_uuid) {
        return {StorageError::schema_invalid,
                "resource target UUID differs from the validated project handle"};
    }
    return Status::success();
}

Status validate_record_shape(const ProjectResourceRecord& record) {
    Status status = validate_identity(record.identity);
    if (!status) return status;

    const auto mode = static_cast<std::uint8_t>(record.storage_mode);
    if (mode > static_cast<std::uint8_t>(ResourceStorageMode::embedded)) {
        return {StorageError::schema_invalid,
                "stored project resource has an invalid storage mode"};
    }
    if (record.chunk_count > static_cast<std::uint64_t>(kMaxEmbeddedResourceChunks)) {
        return {StorageError::schema_invalid,
                "stored project resource exceeds the chunk-count draft bound"};
    }

    if (record.storage_mode == ResourceStorageMode::external) {
        if (record.chunk_count != 0U) {
            return {StorageError::schema_invalid,
                    "external project resource unexpectedly declares embedded chunks"};
        }
    } else if (record.chunk_count != expected_chunk_count(record.identity.size_bytes)) {
        return {StorageError::schema_invalid,
                "embedded project resource chunk_count disagrees with canonical chunking"};
    }
    return Status::success();
}

Status read_record_row(sqlite3_stmt* stmt, ProjectResourceRecord& record) {
    for (const int column : {0, 1, 2, 5}) {
        if (sqlite3_column_type(stmt, column) != SQLITE_TEXT) {
            return {StorageError::schema_invalid,
                    "stored project resource contains invalid text column type"};
        }
    }
    for (const int column : {3, 4, 6, 7}) {
        if (sqlite3_column_type(stmt, column) != SQLITE_INTEGER) {
            return {StorageError::schema_invalid,
                    "stored project resource contains invalid integer column type"};
        }
    }

    const sqlite3_int64 size = sqlite3_column_int64(stmt, 3);
    const sqlite3_int64 mode = sqlite3_column_int64(stmt, 4);
    const sqlite3_int64 required = sqlite3_column_int64(stmt, 6);
    const sqlite3_int64 chunks = sqlite3_column_int64(stmt, 7);
    if (size < 0 || mode < 0 || mode > 1 || required < 0 || required > 1 ||
        chunks < 0 || chunks > static_cast<sqlite3_int64>(kMaxEmbeddedResourceChunks)) {
        return {StorageError::schema_invalid,
                "stored project resource integer is outside canonical range"};
    }

    record.identity.resource_id = text_column(stmt, 0);
    record.identity.sha256 = text_column(stmt, 1);
    record.identity.media_type = text_column(stmt, 2);
    record.identity.size_bytes = static_cast<std::uint64_t>(size);
    record.storage_mode = static_cast<ResourceStorageMode>(static_cast<std::uint8_t>(mode));
    record.identity.retrieval_uri = text_column(stmt, 5);
    record.identity.required_for_reproduction = required == 1;
    record.chunk_count = static_cast<std::uint64_t>(chunks);
    return validate_record_shape(record);
}

Status load_resource(
    sqlite3* db,
    const std::string& resource_id,
    std::optional<ProjectResourceRecord>& record) {
    detail::StmtPtr stmt;
    Status status = detail::prepare(
        db,
        "SELECT resource_id,sha256,media_type,size_bytes,storage_mode,retrieval_uri,required_for_reproduction,chunk_count "
        "FROM aeris_resource WHERE resource_id=?;",
        stmt);
    if (!status) return status;
    if (!(status = detail::bind_text(db, stmt.get(), 1, resource_id))) return status;

    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_DONE) {
        record.reset();
        return Status::success();
    }
    if (rc != SQLITE_ROW) {
        return {StorageError::sqlite_failure,
                detail::sqlite_message(db, "could not read project resource")};
    }

    ProjectResourceRecord loaded{};
    status = read_record_row(stmt.get(), loaded);
    if (!status) return status;
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        return {StorageError::schema_invalid,
                "project resource ID resolves to more than one row"};
    }
    record = std::move(loaded);
    return Status::success();
}

Status load_all_resources(sqlite3* db, std::vector<ProjectResourceRecord>& records) {
    detail::StmtPtr stmt;
    Status status = detail::prepare(
        db,
        "SELECT resource_id,sha256,media_type,size_bytes,storage_mode,retrieval_uri,required_for_reproduction,chunk_count "
        "FROM aeris_resource ORDER BY resource_id;",
        stmt);
    if (!status) return status;

    while (true) {
        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW) {
            return {StorageError::sqlite_failure,
                    detail::sqlite_message(db, "could not enumerate project resources")};
        }
        ProjectResourceRecord record{};
        status = read_record_row(stmt.get(), record);
        if (!status) return status;
        records.push_back(std::move(record));
    }
    return Status::success();
}

Status bind_blob(
    sqlite3* db,
    sqlite3_stmt* stmt,
    const int index,
    const void* data,
    const std::size_t bytes) {
    if (bytes > static_cast<std::size_t>(INT_MAX)) {
        return {StorageError::invalid_argument,
                "embedded resource chunk exceeds SQLite bind size"};
    }
    if (sqlite3_bind_blob(stmt, index, data, static_cast<int>(bytes), SQLITE_TRANSIENT) != SQLITE_OK) {
        return {StorageError::sqlite_failure,
                detail::sqlite_message(db, "sqlite bind resource BLOB failed")};
    }
    return Status::success();
}

Status count_chunks(sqlite3* db, const std::string& resource_id, std::uint64_t& count) {
    detail::StmtPtr stmt;
    Status status = detail::prepare(
        db, "SELECT COUNT(*) FROM aeris_resource_chunk WHERE resource_id=?;", stmt);
    if (!status) return status;
    if (!(status = detail::bind_text(db, stmt.get(), 1, resource_id))) return status;
    const int rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_ROW || sqlite3_column_type(stmt.get(), 0) != SQLITE_INTEGER) {
        return {StorageError::schema_invalid,
                "project resource chunk count query returned malformed data"};
    }
    const sqlite3_int64 raw = sqlite3_column_int64(stmt.get(), 0);
    if (raw < 0) {
        return {StorageError::schema_invalid,
                "project resource chunk count is negative"};
    }
    count = static_cast<std::uint64_t>(raw);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        return {StorageError::schema_invalid,
                "project resource chunk count query returned duplicate rows"};
    }
    return Status::success();
}

Status verify_external_no_chunks(sqlite3* db, const ProjectResourceRecord& record) {
    std::uint64_t actual = 0U;
    Status status = count_chunks(db, record.identity.resource_id, actual);
    if (!status) return status;
    if (actual != 0U) {
        return {StorageError::schema_invalid,
                "external project resource owns unexpected embedded chunk rows"};
    }
    return Status::success();
}

Status verify_embedded(
    sqlite3* db,
    const ProjectResourceRecord& record,
    const EmbeddedResourceConsumer* consumer) {
    if (record.storage_mode != ResourceStorageMode::embedded) {
        return {StorageError::record_not_found,
                "project resource is not embedded"};
    }

    detail::StmtPtr stmt;
    Status status = detail::prepare(
        db,
        "SELECT chunk_index,sha256,payload FROM aeris_resource_chunk "
        "WHERE resource_id=? ORDER BY chunk_index;",
        stmt);
    if (!status) return status;
    if (!(status = detail::bind_text(db, stmt.get(), 1, record.identity.resource_id))) return status;

    aeris::util::Sha256 aggregate{};
    std::uint64_t total_bytes = 0U;
    std::uint64_t seen = 0U;
    while (true) {
        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW || sqlite3_column_type(stmt.get(), 0) != SQLITE_INTEGER ||
            sqlite3_column_type(stmt.get(), 1) != SQLITE_TEXT ||
            sqlite3_column_type(stmt.get(), 2) != SQLITE_BLOB) {
            return {StorageError::schema_invalid,
                    "embedded project resource chunk contains invalid SQLite types"};
        }

        const sqlite3_int64 chunk_index = sqlite3_column_int64(stmt.get(), 0);
        if (chunk_index < 0 || static_cast<std::uint64_t>(chunk_index) != seen) {
            return {StorageError::schema_invalid,
                    "embedded project resource chunk indices are not contiguous from zero"};
        }
        const std::string stored_hash = text_column(stmt.get(), 1);
        if (!is_canonical_sha256(stored_hash)) {
            return {StorageError::schema_invalid,
                    "embedded project resource chunk has noncanonical SHA-256"};
        }

        const int payload_bytes = sqlite3_column_bytes(stmt.get(), 2);
        const void* payload = sqlite3_column_blob(stmt.get(), 2);
        if (payload_bytes <= 0 || payload == nullptr ||
            payload_bytes > static_cast<int>(kEmbeddedResourceChunkBytes)) {
            return {StorageError::schema_invalid,
                    "embedded project resource chunk length is outside canonical bounds"};
        }

        const std::uint64_t remaining = record.identity.size_bytes - total_bytes;
        const std::uint64_t expected = std::min<std::uint64_t>(
            remaining, static_cast<std::uint64_t>(kEmbeddedResourceChunkBytes));
        if (expected == 0U || static_cast<std::uint64_t>(payload_bytes) != expected) {
            return {StorageError::schema_invalid,
                    "embedded project resource chunk length disagrees with canonical size partition"};
        }

        const std::string actual_chunk_hash =
            aeris::util::sha256_bytes(payload, static_cast<std::size_t>(payload_bytes)).hex();
        if (actual_chunk_hash != stored_hash) {
            return {StorageError::integrity_failed,
                    "embedded project resource chunk SHA-256 mismatch"};
        }
        if (aggregate.update(payload, static_cast<std::size_t>(payload_bytes)) !=
            aeris::util::HashError::none) {
            return {StorageError::integrity_failed,
                    "embedded project resource aggregate SHA-256 length overflow"};
        }

        if (consumer != nullptr) {
            status = (*consumer)(payload, static_cast<std::size_t>(payload_bytes));
            if (!status) return status;
        }

        total_bytes += static_cast<std::uint64_t>(payload_bytes);
        ++seen;
    }

    if (seen != record.chunk_count || total_bytes != record.identity.size_bytes) {
        return {StorageError::schema_invalid,
                "embedded project resource chunk rows disagree with declared count/size"};
    }
    const std::string aggregate_hash = aggregate.finalize().hex();
    if (aggregate_hash != record.identity.sha256) {
        return {StorageError::integrity_failed,
                "embedded project resource aggregate SHA-256 mismatch"};
    }
    return Status::success();
}

Status insert_resource_row(
    sqlite3* db,
    const ProjectResourceIdentity& resource,
    const ResourceStorageMode mode,
    const std::uint64_t chunks) {
    detail::StmtPtr stmt;
    Status status = detail::prepare(
        db,
        "INSERT INTO aeris_resource(resource_id,sha256,media_type,size_bytes,storage_mode,retrieval_uri,required_for_reproduction,chunk_count) "
        "VALUES(?,?,?,?,?,?,?,?);",
        stmt);
    if (!status) return status;
    if (!(status = detail::bind_text(db, stmt.get(), 1, resource.resource_id))) return status;
    if (!(status = detail::bind_text(db, stmt.get(), 2, resource.sha256))) return status;
    if (!(status = detail::bind_text(db, stmt.get(), 3, resource.media_type))) return status;
    if (!(status = detail::bind_int64(db, stmt.get(), 4, static_cast<sqlite3_int64>(resource.size_bytes)))) return status;
    if (!(status = detail::bind_int64(db, stmt.get(), 5, static_cast<sqlite3_int64>(static_cast<std::uint8_t>(mode))))) return status;
    if (!(status = detail::bind_text(db, stmt.get(), 6, resource.retrieval_uri))) return status;
    if (!(status = detail::bind_int64(db, stmt.get(), 7, resource.required_for_reproduction ? 1 : 0))) return status;
    if (!(status = detail::bind_int64(db, stmt.get(), 8, static_cast<sqlite3_int64>(chunks)))) return status;
    return detail::step_done(db, stmt.get());
}

Status promote_required(
    sqlite3* db,
    const std::string& resource_id,
    bool& changed) {
    detail::StmtPtr stmt;
    Status status = detail::prepare(
        db,
        "UPDATE aeris_resource SET required_for_reproduction=1 "
        "WHERE resource_id=? AND required_for_reproduction=0;",
        stmt);
    if (status) status = detail::bind_text(db, stmt.get(), 1, resource_id);
    if (status) status = detail::step_done(db, stmt.get());
    if (!status) return status;
    const int affected = sqlite3_changes(db);
    if (affected < 0 || affected > 1) {
        return {StorageError::schema_invalid,
                "resource requirement promotion affected an invalid row count"};
    }
    changed = affected == 1;
    return Status::success();
}

Status mark_resource_embedded(
    sqlite3* db,
    const std::string& resource_id,
    const std::uint64_t chunks) {
    detail::StmtPtr stmt;
    Status status = detail::prepare(
        db,
        "UPDATE aeris_resource SET storage_mode=1,chunk_count=? "
        "WHERE resource_id=? AND storage_mode=0 AND chunk_count=0;",
        stmt);
    if (status) status = detail::bind_int64(db, stmt.get(), 1, static_cast<sqlite3_int64>(chunks));
    if (status) status = detail::bind_text(db, stmt.get(), 2, resource_id);
    if (status) status = detail::step_done(db, stmt.get());
    if (status && sqlite3_changes(db) != 1) {
        return {StorageError::schema_invalid,
                "external resource upgrade did not affect exactly one canonical row"};
    }
    return status;
}

Status read_revision_frozen(
    sqlite3* db,
    std::uint64_t& revision,
    bool& frozen) {
    detail::StmtPtr stmt;
    Status status = detail::prepare(
        db, "SELECT revision,frozen FROM aeris_meta WHERE id=1;", stmt);
    if (!status) return status;
    const int rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_ROW || sqlite3_column_type(stmt.get(), 0) != SQLITE_INTEGER ||
        sqlite3_column_type(stmt.get(), 1) != SQLITE_INTEGER) {
        return {StorageError::schema_invalid,
                "project resource mutation could not read canonical project revision/frozen state"};
    }
    const sqlite3_int64 raw_revision = sqlite3_column_int64(stmt.get(), 0);
    const sqlite3_int64 raw_frozen = sqlite3_column_int64(stmt.get(), 1);
    if (raw_revision < 0 || raw_frozen < 0 || raw_frozen > 1) {
        return {StorageError::schema_invalid,
                "project resource mutation found invalid revision/frozen state"};
    }
    revision = static_cast<std::uint64_t>(raw_revision);
    frozen = raw_frozen == 1;
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        return {StorageError::schema_invalid,
                "aeris_meta is not a singleton during resource mutation"};
    }
    return Status::success();
}

Status advance_project_revision(
    sqlite3* db,
    const ProjectStore& project,
    const std::string_view modified_utc,
    const std::optional<bool> frozen_override = std::nullopt) {
    std::uint64_t current_revision = 0U;
    bool current_frozen = false;
    Status status = read_revision_frozen(db, current_revision, current_frozen);
    if (!status) return status;
    if (current_revision >= static_cast<std::uint64_t>(std::numeric_limits<sqlite3_int64>::max())) {
        return {StorageError::schema_invalid,
                "project revision cannot be incremented for resource mutation"};
    }

    const bool next_frozen = frozen_override.value_or(current_frozen);
    detail::StmtPtr stmt;
    status = detail::prepare(
        db,
        "UPDATE aeris_meta SET revision=?,modified_utc=?,frozen=? WHERE id=1 AND project_uuid=?;",
        stmt);
    if (status) status = detail::bind_int64(
        db, stmt.get(), 1, static_cast<sqlite3_int64>(current_revision + 1U));
    if (status) status = detail::bind_text(db, stmt.get(), 2, std::string(modified_utc));
    if (status) status = detail::bind_int64(db, stmt.get(), 3, next_frozen ? 1 : 0);
    if (status) status = detail::bind_text(db, stmt.get(), 4, project.metadata().project_uuid);
    if (status) status = detail::step_done(db, stmt.get());
    if (status && sqlite3_changes(db) != 1) {
        return {StorageError::schema_invalid,
                "resource mutation could not advance exactly one project metadata row"};
    }
    return status;
}

Status prepare_resource_db(
    ProjectStore& project,
    detail::DbPtr& db) {
    Status status = detail::open_database(project.path(), SQLITE_OPEN_READWRITE, db);
    if (!status) return status;
    if (!(status = validate_project_connection(db.get(), project))) return status;
    return detail::configure_durable(db.get());
}

}  // namespace

ResourceMutationResult store_external_resource(
    ProjectStore& project,
    const ProjectResourceIdentity& resource,
    const std::string_view modified_utc) {
    if (!is_canonical_utc_timestamp(modified_utc)) {
        return {{StorageError::invalid_argument,
                 "resource mutation timestamp is not canonical Gregorian UTC"},
                false, false, false};
    }
    Status status = validate_identity(resource);
    if (!status) return {std::move(status), false, false, false};

    detail::DbPtr db;
    status = prepare_resource_db(project, db);
    if (!status) return {std::move(status), false, false, false};
    if (!(status = detail::begin_immediate(db.get()))) {
        return {std::move(status), false, false, false};
    }

    std::optional<ProjectResourceRecord> existing;
    status = load_resource(db.get(), resource.resource_id, existing);
    if (!status) {
        detail::rollback(db.get());
        return {std::move(status), false, false, false};
    }
    if (existing.has_value()) {
        if (!equal_content_identity(existing->identity, resource)) {
            detail::rollback(db.get());
            return {{StorageError::record_exists,
                     "resource ID already exists with different immutable content identity"},
                    false, false, false};
        }
        status = existing->storage_mode == ResourceStorageMode::embedded
            ? verify_embedded(db.get(), *existing, nullptr)
            : verify_external_no_chunks(db.get(), *existing);
        if (!status) {
            detail::rollback(db.get());
            return {std::move(status), false, false, false};
        }

        bool requirement_changed = false;
        if (resource.required_for_reproduction &&
            !existing->identity.required_for_reproduction) {
            status = promote_required(db.get(), resource.resource_id, requirement_changed);
            if (!status) {
                detail::rollback(db.get());
                return {std::move(status), false, false, false};
            }
        }
        if (!requirement_changed) {
            detail::rollback(db.get());
            status = project.refresh_metadata();
            if (!status) return {std::move(status), false, false, false};
            return {Status::success(), false, false, false};
        }

        std::optional<bool> frozen_override;
        if (existing->storage_mode == ResourceStorageMode::external) frozen_override = false;
        status = advance_project_revision(db.get(), project, modified_utc, frozen_override);
        if (!status) {
            detail::rollback(db.get());
            return {std::move(status), false, false, false};
        }
        status = detail::commit(db.get());
        if (!status) {
            detail::rollback(db.get());
            return {std::move(status), false, false, false};
        }
        status = project.refresh_metadata();
        if (!status) return {std::move(status), false, false, true};
        return {Status::success(), false, false, true};
    }

    status = insert_resource_row(db.get(), resource, ResourceStorageMode::external, 0U);
    if (status) {
        std::optional<bool> frozen_override;
        if (resource.required_for_reproduction) frozen_override = false;
        status = advance_project_revision(db.get(), project, modified_utc, frozen_override);
    }
    if (!status) {
        detail::rollback(db.get());
        return {std::move(status), false, false, false};
    }
    status = detail::commit(db.get());
    if (!status) {
        detail::rollback(db.get());
        return {std::move(status), false, false, false};
    }

    status = project.refresh_metadata();
    if (!status) return {std::move(status), true, false, true};
    return {Status::success(), true, false, true};
}

ResourceMutationResult embed_resource_file(
    ProjectStore& project,
    const ProjectResourceIdentity& resource,
    const std::filesystem::path& input_path,
    const std::string_view modified_utc) {
    if (!is_canonical_utc_timestamp(modified_utc) || input_path.empty()) {
        return {{StorageError::invalid_argument,
                 "embedded resource requires a canonical mutation timestamp and non-empty input path"},
                false, false, false};
    }
    Status status = validate_identity(resource);
    if (!status) return {std::move(status), false, false, false};

    std::error_code ec;
    if (!std::filesystem::is_regular_file(input_path, ec) || ec) {
        return {{StorageError::file_not_found,
                 "embedded resource input is not a readable regular file"},
                false, false, false};
    }
    const std::uintmax_t file_size = std::filesystem::file_size(input_path, ec);
    if (ec || file_size != static_cast<std::uintmax_t>(resource.size_bytes)) {
        return {{StorageError::integrity_failed,
                 "embedded resource input size differs from declared immutable identity"},
                false, false, false};
    }

    std::error_code equivalent_error;
    if (std::filesystem::equivalent(input_path, project.path(), equivalent_error) && !equivalent_error) {
        return {{StorageError::invalid_argument,
                 "project database itself cannot be embedded as one of its resources"},
                false, false, false};
    }

    std::ifstream input(input_path, std::ios::binary);
    if (!input) {
        return {{StorageError::filesystem_failure,
                 "could not open embedded resource input"},
                false, false, false};
    }

    detail::DbPtr db;
    status = prepare_resource_db(project, db);
    if (!status) return {std::move(status), false, false, false};
    if (!(status = detail::begin_immediate(db.get()))) {
        return {std::move(status), false, false, false};
    }

    std::optional<ProjectResourceRecord> existing;
    status = load_resource(db.get(), resource.resource_id, existing);
    if (!status) {
        detail::rollback(db.get());
        return {std::move(status), false, false, false};
    }
    if (existing.has_value() && !equal_content_identity(existing->identity, resource)) {
        detail::rollback(db.get());
        return {{StorageError::record_exists,
                 "resource ID already exists with different immutable content identity"},
                false, false, false};
    }

    bool requirement_changed = false;
    if (existing.has_value() && resource.required_for_reproduction &&
        !existing->identity.required_for_reproduction) {
        status = promote_required(db.get(), resource.resource_id, requirement_changed);
        if (!status) {
            detail::rollback(db.get());
            return {std::move(status), false, false, false};
        }
        existing->identity.required_for_reproduction = true;
    }

    if (existing.has_value() && existing->storage_mode == ResourceStorageMode::embedded) {
        status = verify_embedded(db.get(), *existing, nullptr);
        if (!status) {
            detail::rollback(db.get());
            return {std::move(status), false, false, false};
        }
        if (!requirement_changed) {
            detail::rollback(db.get());
            status = project.refresh_metadata();
            if (!status) return {std::move(status), false, false, false};
            return {Status::success(), false, false, false};
        }
        status = advance_project_revision(db.get(), project, modified_utc);
        if (!status) {
            detail::rollback(db.get());
            return {std::move(status), false, false, false};
        }
        status = detail::commit(db.get());
        if (!status) {
            detail::rollback(db.get());
            return {std::move(status), false, false, false};
        }
        status = project.refresh_metadata();
        if (!status) return {std::move(status), false, false, true};
        return {Status::success(), false, false, true};
    }
    if (existing.has_value()) {
        status = verify_external_no_chunks(db.get(), *existing);
        if (!status) {
            detail::rollback(db.get());
            return {std::move(status), false, false, false};
        }
    }

    const std::uint64_t chunks = expected_chunk_count(resource.size_bytes);
    if (existing.has_value()) {
        status = mark_resource_embedded(db.get(), resource.resource_id, chunks);
    } else {
        status = insert_resource_row(db.get(), resource, ResourceStorageMode::embedded, chunks);
    }
    if (!status) {
        detail::rollback(db.get());
        return {std::move(status), false, false, false};
    }

    detail::StmtPtr chunk_stmt;
    status = detail::prepare(
        db.get(),
        "INSERT INTO aeris_resource_chunk(resource_id,chunk_index,sha256,payload) VALUES(?,?,?,?);",
        chunk_stmt);
    if (!status) {
        detail::rollback(db.get());
        return {std::move(status), false, false, false};
    }

    std::vector<unsigned char> buffer(kEmbeddedResourceChunkBytes);
    aeris::util::Sha256 aggregate{};
    std::uint64_t remaining = resource.size_bytes;
    for (std::uint64_t index = 0U; index < chunks; ++index) {
        const std::size_t wanted = static_cast<std::size_t>(std::min<std::uint64_t>(
            remaining, static_cast<std::uint64_t>(kEmbeddedResourceChunkBytes)));
        input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(wanted));
        if (input.gcount() != static_cast<std::streamsize>(wanted)) {
            detail::rollback(db.get());
            return {{StorageError::integrity_failed,
                     "embedded resource input changed or ended before declared size"},
                    false, false, false};
        }

        if (aggregate.update(buffer.data(), wanted) != aeris::util::HashError::none) {
            detail::rollback(db.get());
            return {{StorageError::integrity_failed,
                     "embedded resource SHA-256 length overflow"},
                    false, false, false};
        }
        const std::string chunk_hash = aeris::util::sha256_bytes(buffer.data(), wanted).hex();

        if (sqlite3_reset(chunk_stmt.get()) != SQLITE_OK ||
            sqlite3_clear_bindings(chunk_stmt.get()) != SQLITE_OK) {
            detail::rollback(db.get());
            return {{StorageError::sqlite_failure,
                     detail::sqlite_message(db.get(), "could not reset embedded resource chunk statement")},
                    false, false, false};
        }
        if (!(status = detail::bind_text(db.get(), chunk_stmt.get(), 1, resource.resource_id)) ||
            !(status = detail::bind_int64(db.get(), chunk_stmt.get(), 2, static_cast<sqlite3_int64>(index))) ||
            !(status = detail::bind_text(db.get(), chunk_stmt.get(), 3, chunk_hash)) ||
            !(status = bind_blob(db.get(), chunk_stmt.get(), 4, buffer.data(), wanted)) ||
            !(status = detail::step_done(db.get(), chunk_stmt.get()))) {
            detail::rollback(db.get());
            return {std::move(status), false, false, false};
        }
        remaining -= static_cast<std::uint64_t>(wanted);
    }

    char extra = 0;
    if (input.get(extra)) {
        detail::rollback(db.get());
        return {{StorageError::integrity_failed,
                 "embedded resource input grew beyond declared immutable size"},
                false, false, false};
    }
    if (remaining != 0U || aggregate.finalize().hex() != resource.sha256) {
        detail::rollback(db.get());
        return {{StorageError::integrity_failed,
                 "embedded resource aggregate SHA-256 differs from declared immutable identity"},
                false, false, false};
    }

    status = advance_project_revision(db.get(), project, modified_utc);
    if (!status) {
        detail::rollback(db.get());
        return {std::move(status), false, false, false};
    }
    status = detail::commit(db.get());
    if (!status) {
        detail::rollback(db.get());
        return {std::move(status), false, false, false};
    }

    const bool inserted = !existing.has_value();
    const bool representation_changed = existing.has_value();
    status = project.refresh_metadata();
    if (!status) return {std::move(status), inserted, representation_changed, true};
    return {Status::success(), inserted, representation_changed, true};
}

ProjectResourceListResult list_project_resources(const ProjectStore& project) {
    detail::DbPtr db;
    Status status = detail::open_database(project.path(), SQLITE_OPEN_READONLY, db);
    if (!status) return {std::move(status), {}};
    if (!(status = validate_project_connection(db.get(), project))) {
        return {std::move(status), {}};
    }

    ProjectResourceListResult result{};
    status = load_all_resources(db.get(), result.records);
    result.status = std::move(status);
    if (!result.status) result.records.clear();
    return result;
}

Status stream_embedded_resource(
    const ProjectStore& project,
    const std::string_view resource_id,
    const EmbeddedResourceConsumer& consumer) {
    if (resource_id.empty() || resource_id.size() > kMaxIdentifierBytes ||
        resource_id.find('\0') != std::string_view::npos || !consumer) {
        return {StorageError::invalid_argument,
                "embedded resource stream requires a canonical ID and consumer"};
    }

    detail::DbPtr db;
    Status status = detail::open_database(project.path(), SQLITE_OPEN_READONLY, db);
    if (!status) return status;
    if (!(status = validate_project_connection(db.get(), project))) return status;

    std::optional<ProjectResourceRecord> record;
    status = load_resource(db.get(), std::string(resource_id), record);
    if (!status) return status;
    if (!record.has_value()) {
        return {StorageError::record_not_found,
                "project resource ID was not found"};
    }
    return verify_embedded(db.get(), *record, &consumer);
}

ProjectFreezeResult freeze_project(
    ProjectStore& project,
    const std::string_view modified_utc) {
    if (!is_canonical_utc_timestamp(modified_utc)) {
        return {{StorageError::invalid_argument,
                 "freeze timestamp is not canonical Gregorian UTC"},
                false, false};
    }

    detail::DbPtr db;
    Status status = prepare_resource_db(project, db);
    if (!status) return {std::move(status), false, false};
    if (!(status = detail::begin_immediate(db.get()))) {
        return {std::move(status), false, false};
    }

    std::vector<ProjectResourceRecord> resources;
    status = load_all_resources(db.get(), resources);
    if (!status) {
        detail::rollback(db.get());
        return {std::move(status), false, false};
    }

    for (const ProjectResourceRecord& resource : resources) {
        if (resource.identity.required_for_reproduction &&
            resource.storage_mode != ResourceStorageMode::embedded) {
            detail::rollback(db.get());
            return {{StorageError::invalid_argument,
                     "project cannot be frozen while required resource remains external: " +
                         resource.identity.resource_id},
                    false, false};
        }
        status = resource.storage_mode == ResourceStorageMode::embedded
            ? verify_embedded(db.get(), resource, nullptr)
            : verify_external_no_chunks(db.get(), resource);
        if (!status) {
            detail::rollback(db.get());
            return {std::move(status), false, false};
        }
    }

    std::uint64_t revision = 0U;
    bool frozen = false;
    status = read_revision_frozen(db.get(), revision, frozen);
    if (!status) {
        detail::rollback(db.get());
        return {std::move(status), false, false};
    }
    if (frozen) {
        detail::rollback(db.get());
        status = project.refresh_metadata();
        if (!status) return {std::move(status), false, false};
        return {Status::success(), false, false};
    }

    status = advance_project_revision(db.get(), project, modified_utc, true);
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

namespace detail {

Status verify_resource_semantics(const ProjectStore& project) {
    DbPtr db;
    Status status = open_database(project.path(), SQLITE_OPEN_READONLY, db);
    if (!status) return status;
    if (!(status = validate_project_connection(db.get(), project))) return status;

    std::vector<ProjectResourceRecord> resources;
    status = load_all_resources(db.get(), resources);
    if (!status) return status;

    std::uint64_t revision = 0U;
    bool frozen = false;
    status = read_revision_frozen(db.get(), revision, frozen);
    if (!status) return status;
    (void)revision;

    for (const ProjectResourceRecord& resource : resources) {
        if (frozen && resource.identity.required_for_reproduction &&
            resource.storage_mode != ResourceStorageMode::embedded) {
            return {StorageError::integrity_failed,
                    "frozen project references a required external resource: " +
                        resource.identity.resource_id};
        }
        status = resource.storage_mode == ResourceStorageMode::embedded
            ? verify_embedded(db.get(), resource, nullptr)
            : verify_external_no_chunks(db.get(), resource);
        if (!status) return status;
    }
    return Status::success();
}

}  // namespace detail
}  // namespace aeris::storage
