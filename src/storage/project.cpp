// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#include "aeris/storage/project.hpp"

#include "geometry_detail.hpp"
#include "layer_detail.hpp"
#include "resource_detail.hpp"
#include "sqlite_detail.hpp"

#include <array>
#include <limits>
#include <system_error>
#include <utility>

#include <sqlite3.h>

namespace aeris::storage {
namespace {

constexpr std::size_t kMaxMetadataText = 255U;
constexpr int kStagingDirectoryAttempts = 8;

bool is_hex(const char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool valid_small_text(const std::string& value) noexcept {
    return !value.empty() && value.size() <= kMaxMetadataText && value.find('\0') == std::string::npos;
}

int two_digits(std::string_view value, const std::size_t offset) noexcept {
    const char a = value[offset];
    const char b = value[offset + 1U];
    if (a < '0' || a > '9' || b < '0' || b > '9') return -1;
    return (a - '0') * 10 + (b - '0');
}

int four_digits(std::string_view value, const std::size_t offset) noexcept {
    int result = 0;
    for (std::size_t i = 0; i < 4U; ++i) {
        const char c = value[offset + i];
        if (c < '0' || c > '9') return -1;
        result = result * 10 + (c - '0');
    }
    return result;
}

bool is_gregorian_leap_year(const int year) noexcept {
    return (year % 4 == 0) && ((year % 100 != 0) || (year % 400 == 0));
}

int days_in_gregorian_month(const int year, const int month) noexcept {
    constexpr std::array<int, 12> days{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 0;
    if (month == 2 && is_gregorian_leap_year(year)) return 29;
    return days[static_cast<std::size_t>(month - 1)];
}

std::string generated_uuid_v4() {
    std::array<unsigned char, 16> bytes{};
    sqlite3_randomness(static_cast<int>(bytes.size()), bytes.data());
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0FU) | 0x40U);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3FU) | 0x80U);

    constexpr char digits[] = "0123456789abcdef";
    std::string value;
    value.reserve(36U);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4U || i == 6U || i == 8U || i == 10U) value.push_back('-');
        value.push_back(digits[(bytes[i] >> 4U) & 0x0FU]);
        value.push_back(digits[bytes[i] & 0x0FU]);
    }
    return value;
}

Status make_staging_directory(const std::filesystem::path& destination, std::filesystem::path& staging) {
    std::filesystem::path parent = destination.parent_path();
    if (parent.empty()) parent = ".";

    for (int attempt = 0; attempt < kStagingDirectoryAttempts; ++attempt) {
        const std::filesystem::path candidate = parent / (".aeris-create-" + generated_uuid_v4());
        std::error_code ec;
        if (std::filesystem::create_directory(candidate, ec)) {
            staging = candidate;
            return Status::success();
        }
        if (ec && ec != std::make_error_code(std::errc::file_exists)) {
            return {StorageError::filesystem_failure,
                    "could not create sibling project staging directory: " + ec.message()};
        }
    }
    return {StorageError::filesystem_failure,
            "could not allocate a unique sibling project staging directory"};
}

void remove_staging_directory(const std::filesystem::path& staging) noexcept {
    std::error_code ignored;
    (void)std::filesystem::remove_all(staging, ignored);
}

Status validate_create_options(const ProjectCreateOptions& options) {
    if (!is_canonical_utc_timestamp(options.timestamp_utc)) {
        return {StorageError::invalid_argument,
                "project creation timestamp must be canonical UTC YYYY-MM-DDTHH:MM:SSZ"};
    }
    if (!options.project_uuid.empty() && !is_canonical_uuid(options.project_uuid)) {
        return {StorageError::invalid_project_uuid,
                "project UUID is not canonical 8-4-4-4-12 hexadecimal form"};
    }
    if (!valid_small_text(options.producer) || !valid_small_text(options.producer_version) ||
        !valid_small_text(options.projection_id) || !valid_small_text(options.worldview_id)) {
        return {StorageError::invalid_argument,
                "project metadata identifiers must be non-empty, NUL-free, and at most 255 bytes"};
    }
    if (options.frozen) {
        return {StorageError::invalid_argument,
                "project creation cannot assert frozen state; create first and use verified freeze_project()"};
    }
    return Status::success();
}

Status validate_metadata_update(const ProjectMetadataUpdate& update) {
    if (!is_canonical_utc_timestamp(update.modified_utc)) {
        return {StorageError::invalid_argument,
                "project mutation timestamp must be canonical UTC YYYY-MM-DDTHH:MM:SSZ"};
    }
    if (update.projection_id && !valid_small_text(*update.projection_id)) {
        return {StorageError::invalid_argument,
                "projection identifier must be non-empty, NUL-free, and at most 255 bytes"};
    }
    if (update.worldview_id && !valid_small_text(*update.worldview_id)) {
        return {StorageError::invalid_argument,
                "worldview identifier must be non-empty, NUL-free, and at most 255 bytes"};
    }
    if (update.frozen.has_value()) {
        return {StorageError::invalid_argument,
                "frozen is a verified resource-state invariant and cannot be changed through metadata update"};
    }
    if (!update.projection_id && !update.worldview_id) {
        return {StorageError::invalid_argument,
                "metadata update contains no acknowledged project mutation"};
    }
    return Status::success();
}

Status validate_identity(sqlite3* db) {
    sqlite3_int64 application_id = 0;
    Status status = detail::query_single_int(db, "PRAGMA application_id;", application_id);
    if (!status) return status;
    if (application_id != static_cast<sqlite3_int64>(kProjectApplicationId)) {
        return {StorageError::invalid_application_id,
                "file is not an AERIS project (application_id mismatch)"};
    }

    sqlite3_int64 schema_generation = 0;
    status = detail::query_single_int(db, "PRAGMA user_version;", schema_generation);
    if (!status) return status;
    if (schema_generation != kProjectSchemaGeneration) {
        return {StorageError::unsupported_schema,
                "unsupported AERIS draft project schema generation"};
    }
    return Status::success();
}

Status validate_schema_surface(sqlite3* db) {
    for (const char* sql : {
             "SELECT source_id,adapter_id,capability_bits,temporal_class,provider,dataset,snapshot,dataset_version,source_uri,license_id,content_sha256,retrieved_at_utc,worldview FROM aeris_source LIMIT 0;",
             "SELECT source_id,logical_name,sha256,size_bytes FROM aeris_source_resource LIMIT 0;",
             "SELECT source_id,model_id,encoding_id,feature_count FROM aeris_source_geometry LIMIT 0;",
             "SELECT source_id,stable_id,source_feature_id,ring_count FROM aeris_feature LIMIT 0;",
             "SELECT source_id,stable_id,ring_index,role,interior_side,longitude_winding,closing_longitude_f64le,vertex_count,vertices_f64le FROM aeris_feature_ring LIMIT 0;",
             "SELECT resource_id,sha256,media_type,size_bytes,storage_mode,retrieval_uri,required_for_reproduction,chunk_count FROM aeris_resource LIMIT 0;",
             "SELECT resource_id,chunk_index,sha256,payload FROM aeris_resource_chunk LIMIT 0;",
             "SELECT layer_id,role_id,name,ordinal,visible FROM aeris_layer LIMIT 0;",
             "SELECT layer_id,slot_id,source_id FROM aeris_layer_source LIMIT 0;",
             "SELECT layer_id,slot_id,resource_id FROM aeris_layer_resource LIMIT 0;"}) {
        detail::StmtPtr stmt;
        Status status = detail::prepare(db, sql, stmt);
        if (!status) {
            return {StorageError::schema_invalid,
                    "required project schema surface is missing: " + status.diagnostic};
        }
        const int rc = sqlite3_step(stmt.get());
        if (rc != SQLITE_DONE) {
            return {StorageError::schema_invalid,
                    "required project schema probe did not terminate cleanly"};
        }
    }

    detail::StmtPtr fk;
    Status status = detail::prepare(db, "PRAGMA foreign_key_check;", fk);
    if (!status) return status;
    const int rc = sqlite3_step(fk.get());
    if (rc == SQLITE_ROW) {
        return {StorageError::integrity_failed,
                "project foreign-key integrity check failed"};
    }
    if (rc != SQLITE_DONE) {
        return {StorageError::sqlite_failure,
                detail::sqlite_message(db, "foreign-key integrity check failed")};
    }
    return Status::success();
}

Status load_metadata(sqlite3* db, ProjectMetadata& metadata) {
    detail::StmtPtr stmt;
    Status status = detail::prepare(
        db,
        "SELECT project_uuid,format_major,format_minor,revision,created_utc,modified_utc,"
        "producer,producer_version,projection_id,worldview_id,frozen FROM aeris_meta WHERE id=1;",
        stmt);
    if (!status) return status;

    const int rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_ROW) {
        return {StorageError::schema_invalid,
                "aeris_meta singleton row is missing"};
    }
    for (int column : {0, 4, 5, 6, 7, 8, 9}) {
        if (sqlite3_column_type(stmt.get(), column) != SQLITE_TEXT) {
            return {StorageError::schema_invalid,
                    "aeris_meta contains a non-text value in a required text column"};
        }
    }
    for (int column : {1, 2, 3, 10}) {
        if (sqlite3_column_type(stmt.get(), column) != SQLITE_INTEGER) {
            return {StorageError::schema_invalid,
                    "aeris_meta contains a non-integer value in a required integer column"};
        }
    }

    const auto text_at = [&](const int column) {
        const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), column));
        const int bytes = sqlite3_column_bytes(stmt.get(), column);
        return std::string(text, static_cast<std::size_t>(bytes));
    };

    metadata.project_uuid = text_at(0);
    metadata.format_major = sqlite3_column_int(stmt.get(), 1);
    metadata.format_minor = sqlite3_column_int(stmt.get(), 2);
    const sqlite3_int64 revision = sqlite3_column_int64(stmt.get(), 3);
    metadata.created_utc = text_at(4);
    metadata.modified_utc = text_at(5);
    metadata.producer = text_at(6);
    metadata.producer_version = text_at(7);
    metadata.projection_id = text_at(8);
    metadata.worldview_id = text_at(9);
    const int frozen = sqlite3_column_int(stmt.get(), 10);

    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        return {StorageError::schema_invalid,
                "aeris_meta contains more than the singleton row"};
    }
    if (!is_canonical_uuid(metadata.project_uuid)) {
        return {StorageError::invalid_project_uuid,
                "stored project UUID is invalid"};
    }
    if (metadata.format_major != kDraftFormatMajor || metadata.format_minor != kDraftFormatMinor) {
        return {StorageError::unsupported_schema,
                "unsupported AERIS draft format version"};
    }
    if (revision < 0) {
        return {StorageError::schema_invalid,
                "project revision is negative"};
    }
    if (!is_canonical_utc_timestamp(metadata.created_utc) ||
        !is_canonical_utc_timestamp(metadata.modified_utc)) {
        return {StorageError::schema_invalid,
                "project timestamps are not canonical UTC values"};
    }
    if (!valid_small_text(metadata.producer) || !valid_small_text(metadata.producer_version) ||
        !valid_small_text(metadata.projection_id) || !valid_small_text(metadata.worldview_id)) {
        return {StorageError::schema_invalid,
                "project metadata identifier violates storage bounds"};
    }
    if (frozen != 0 && frozen != 1) {
        return {StorageError::schema_invalid,
                "project frozen flag is not boolean"};
    }
    metadata.revision = static_cast<std::uint64_t>(revision);
    metadata.frozen = frozen == 1;
    return Status::success();
}

Status validate_frozen_claim(sqlite3* db, const ProjectMetadata& metadata) {
    if (!metadata.frozen) return Status::success();

    detail::StmtPtr stmt;
    Status status = detail::prepare(
        db,
        "SELECT resource_id FROM aeris_resource "
        "WHERE required_for_reproduction=1 AND storage_mode<>1 LIMIT 1;",
        stmt);
    if (!status) return status;
    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_DONE) return Status::success();
    if (rc != SQLITE_ROW || sqlite3_column_type(stmt.get(), 0) != SQLITE_TEXT) {
        return {StorageError::schema_invalid,
                "frozen project resource probe returned malformed data"};
    }
    return {StorageError::schema_invalid,
            "project claims frozen state while a required resource remains external"};
}

Status insert_metadata(sqlite3* db, const ProjectMetadata& metadata) {
    detail::StmtPtr stmt;
    Status status = detail::prepare(
        db,
        "INSERT INTO aeris_meta(id,project_uuid,format_major,format_minor,revision,created_utc,modified_utc,"
        "producer,producer_version,projection_id,worldview_id,frozen) VALUES(1,?,?,?,?,?,?,?,?,?,?,?);",
        stmt);
    if (!status) return status;

    int index = 1;
    if (!(status = detail::bind_text(db, stmt.get(), index++, metadata.project_uuid))) return status;
    if (!(status = detail::bind_int64(db, stmt.get(), index++, metadata.format_major))) return status;
    if (!(status = detail::bind_int64(db, stmt.get(), index++, metadata.format_minor))) return status;
    if (!(status = detail::bind_int64(db, stmt.get(), index++, 0))) return status;
    if (!(status = detail::bind_text(db, stmt.get(), index++, metadata.created_utc))) return status;
    if (!(status = detail::bind_text(db, stmt.get(), index++, metadata.modified_utc))) return status;
    if (!(status = detail::bind_text(db, stmt.get(), index++, metadata.producer))) return status;
    if (!(status = detail::bind_text(db, stmt.get(), index++, metadata.producer_version))) return status;
    if (!(status = detail::bind_text(db, stmt.get(), index++, metadata.projection_id))) return status;
    if (!(status = detail::bind_text(db, stmt.get(), index++, metadata.worldview_id))) return status;
    if (!(status = detail::bind_int64(db, stmt.get(), index, metadata.frozen ? 1 : 0))) return status;
    return detail::step_done(db, stmt.get());
}

}  // namespace

class ProjectStore::Impl {
public:
    detail::DbPtr db;
    std::filesystem::path path;
    ProjectMetadata metadata;
};

bool is_canonical_uuid(const std::string_view value) noexcept {
    if (value.size() != 36U) return false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (i == 8U || i == 13U || i == 18U || i == 23U) {
            if (value[i] != '-') return false;
        } else if (!is_hex(value[i])) {
            return false;
        }
    }
    return true;
}

bool is_canonical_utc_timestamp(const std::string_view value) noexcept {
    if (value.size() != 20U || value[4] != '-' || value[7] != '-' || value[10] != 'T' ||
        value[13] != ':' || value[16] != ':' || value[19] != 'Z') {
        return false;
    }
    for (const std::size_t i : {0U, 1U, 2U, 3U, 5U, 6U, 8U, 9U, 11U, 12U, 14U, 15U, 17U, 18U}) {
        if (value[i] < '0' || value[i] > '9') return false;
    }
    const int year = four_digits(value, 0U);
    const int month = two_digits(value, 5U);
    const int day = two_digits(value, 8U);
    const int hour = two_digits(value, 11U);
    const int minute = two_digits(value, 14U);
    const int second = two_digits(value, 17U);
    const int month_days = days_in_gregorian_month(year, month);
    return year >= 0 && month_days != 0 && day >= 1 && day <= month_days &&
           hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59 && second >= 0 && second <= 59;
}

ProjectStore::ProjectStore(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
ProjectStore::~ProjectStore() = default;
ProjectStore::ProjectStore(ProjectStore&&) noexcept = default;
ProjectStore& ProjectStore::operator=(ProjectStore&&) noexcept = default;

ProjectStoreResult ProjectStore::create(
    const std::filesystem::path& path,
    const ProjectCreateOptions& options) {
    if (path.empty()) {
        return {{StorageError::invalid_argument, "project path is empty"}, nullptr};
    }
    Status status = validate_create_options(options);
    if (!status) return {std::move(status), nullptr};

    std::filesystem::path staging;
    status = make_staging_directory(path, staging);
    if (!status) return {std::move(status), nullptr};
    const std::filesystem::path staged_path = staging / "project.sqlite";

    detail::DbPtr staged_db;
    status = detail::open_database(
        staged_path, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, staged_db);
    if (!status) {
        remove_staging_directory(staging);
        return {std::move(status), nullptr};
    }

    if (!(status = detail::configure_durable(staged_db.get())) ||
        !(status = detail::begin_immediate(staged_db.get()))) {
        staged_db.reset();
        remove_staging_directory(staging);
        return {std::move(status), nullptr};
    }

    const char* schema =
        "PRAGMA application_id=1095062089;"
        "PRAGMA user_version=5;"
        "CREATE TABLE aeris_meta("
        "id INTEGER PRIMARY KEY CHECK(id=1),"
        "project_uuid TEXT NOT NULL,"
        "format_major INTEGER NOT NULL,"
        "format_minor INTEGER NOT NULL,"
        "revision INTEGER NOT NULL CHECK(revision>=0),"
        "created_utc TEXT NOT NULL,"
        "modified_utc TEXT NOT NULL,"
        "producer TEXT NOT NULL,"
        "producer_version TEXT NOT NULL,"
        "projection_id TEXT NOT NULL,"
        "worldview_id TEXT NOT NULL,"
        "frozen INTEGER NOT NULL CHECK(frozen IN (0,1))"
        ");"
        "CREATE TABLE aeris_source("
        "source_id TEXT PRIMARY KEY,"
        "adapter_id TEXT NOT NULL,"
        "capability_bits INTEGER NOT NULL CHECK(capability_bits>0 AND capability_bits<=4294967295),"
        "temporal_class INTEGER NOT NULL CHECK(temporal_class>=0 AND temporal_class<=255),"
        "provider TEXT NOT NULL,"
        "dataset TEXT NOT NULL,"
        "snapshot TEXT NOT NULL,"
        "dataset_version TEXT NOT NULL,"
        "source_uri TEXT NOT NULL,"
        "license_id TEXT NOT NULL,"
        "content_sha256 TEXT NOT NULL,"
        "retrieved_at_utc TEXT NOT NULL,"
        "worldview TEXT NOT NULL"
        ");"
        "CREATE TABLE aeris_source_resource("
        "source_id TEXT NOT NULL REFERENCES aeris_source(source_id) ON DELETE CASCADE,"
        "logical_name TEXT NOT NULL,"
        "sha256 TEXT NOT NULL,"
        "size_bytes INTEGER CHECK(size_bytes IS NULL OR size_bytes>=0),"
        "PRIMARY KEY(source_id,logical_name)"
        ");"
        "CREATE TABLE aeris_source_geometry("
        "source_id TEXT PRIMARY KEY REFERENCES aeris_source(source_id) ON DELETE CASCADE,"
        "model_id TEXT NOT NULL,"
        "encoding_id TEXT NOT NULL,"
        "feature_count INTEGER NOT NULL CHECK(feature_count>=0 AND feature_count<=1000000)"
        ");"
        "CREATE TABLE aeris_feature("
        "source_id TEXT NOT NULL REFERENCES aeris_source_geometry(source_id) ON DELETE CASCADE,"
        "stable_id TEXT NOT NULL,"
        "source_feature_id TEXT NOT NULL,"
        "ring_count INTEGER NOT NULL CHECK(ring_count>=1 AND ring_count<=65535),"
        "PRIMARY KEY(source_id,stable_id),"
        "UNIQUE(source_id,source_feature_id)"
        ");"
        "CREATE TABLE aeris_feature_ring("
        "source_id TEXT NOT NULL,"
        "stable_id TEXT NOT NULL,"
        "ring_index INTEGER NOT NULL CHECK(ring_index>=0 AND ring_index<65535),"
        "role INTEGER NOT NULL CHECK(role IN (0,1)),"
        "interior_side INTEGER NOT NULL CHECK(interior_side IN (0,1,2)),"
        "longitude_winding INTEGER NOT NULL CHECK(longitude_winding>=-2147483648 AND longitude_winding<=2147483647),"
        "closing_longitude_f64le BLOB NOT NULL CHECK(length(closing_longitude_f64le)=8),"
        "vertex_count INTEGER NOT NULL CHECK(vertex_count>=3 AND vertex_count<=4194304),"
        "vertices_f64le BLOB NOT NULL,"
        "PRIMARY KEY(source_id,stable_id,ring_index),"
        "FOREIGN KEY(source_id,stable_id) REFERENCES aeris_feature(source_id,stable_id) ON DELETE CASCADE,"
        "CHECK(length(vertices_f64le)=vertex_count*16)"
        ");"
        "CREATE TABLE aeris_resource("
        "resource_id TEXT PRIMARY KEY,"
        "sha256 TEXT NOT NULL,"
        "media_type TEXT NOT NULL,"
        "size_bytes INTEGER NOT NULL CHECK(size_bytes>=0),"
        "storage_mode INTEGER NOT NULL CHECK(storage_mode IN (0,1)),"
        "retrieval_uri TEXT NOT NULL,"
        "required_for_reproduction INTEGER NOT NULL CHECK(required_for_reproduction IN (0,1)),"
        "chunk_count INTEGER NOT NULL CHECK(chunk_count>=0 AND chunk_count<=4194304),"
        "CHECK(storage_mode=1 OR chunk_count=0)"
        ");"
        "CREATE TABLE aeris_resource_chunk("
        "resource_id TEXT NOT NULL REFERENCES aeris_resource(resource_id) ON DELETE CASCADE,"
        "chunk_index INTEGER NOT NULL CHECK(chunk_index>=0 AND chunk_index<4194304),"
        "sha256 TEXT NOT NULL,"
        "payload BLOB NOT NULL CHECK(length(payload)>0 AND length(payload)<=1048576),"
        "PRIMARY KEY(resource_id,chunk_index)"
        ");"
        "CREATE TABLE aeris_layer("
        "layer_id TEXT PRIMARY KEY,"
        "role_id TEXT NOT NULL,"
        "name TEXT NOT NULL,"
        "ordinal INTEGER NOT NULL UNIQUE CHECK(ordinal>=0),"
        "visible INTEGER NOT NULL CHECK(visible IN (0,1))"
        ");"
        "CREATE TABLE aeris_layer_source("
        "layer_id TEXT NOT NULL REFERENCES aeris_layer(layer_id) ON DELETE CASCADE,"
        "slot_id TEXT NOT NULL,"
        "source_id TEXT NOT NULL REFERENCES aeris_source(source_id),"
        "PRIMARY KEY(layer_id,slot_id)"
        ");"
        "CREATE TABLE aeris_layer_resource("
        "layer_id TEXT NOT NULL REFERENCES aeris_layer(layer_id) ON DELETE CASCADE,"
        "slot_id TEXT NOT NULL,"
        "resource_id TEXT NOT NULL REFERENCES aeris_resource(resource_id),"
        "PRIMARY KEY(layer_id,slot_id)"
        ");";
    status = detail::exec(staged_db.get(), schema);
    if (!status) {
        detail::rollback(staged_db.get());
        staged_db.reset();
        remove_staging_directory(staging);
        return {std::move(status), nullptr};
    }

    ProjectMetadata metadata;
    metadata.project_uuid = options.project_uuid.empty() ? generated_uuid_v4() : options.project_uuid;
    metadata.created_utc = options.timestamp_utc;
    metadata.modified_utc = options.timestamp_utc;
    metadata.producer = options.producer;
    metadata.producer_version = options.producer_version;
    metadata.projection_id = options.projection_id;
    metadata.worldview_id = options.worldview_id;
    metadata.frozen = false;

    status = insert_metadata(staged_db.get(), metadata);
    if (!status) {
        detail::rollback(staged_db.get());
        staged_db.reset();
        remove_staging_directory(staging);
        return {std::move(status), nullptr};
    }
    status = detail::commit(staged_db.get());
    if (!status) {
        detail::rollback(staged_db.get());
        staged_db.reset();
        remove_staging_directory(staging);
        return {std::move(status), nullptr};
    }
    status = detail::verify_quick_check(staged_db.get());
    if (!status) {
        staged_db.reset();
        remove_staging_directory(staging);
        return {std::move(status), nullptr};
    }
    if (!(status = validate_schema_surface(staged_db.get()))) {
        staged_db.reset();
        remove_staging_directory(staging);
        return {std::move(status), nullptr};
    }
    staged_db.reset();

    std::error_code publish_error;
    std::filesystem::create_hard_link(staged_path, path, publish_error);
    if (publish_error) {
        std::error_code inspect_error;
        const bool destination_exists = std::filesystem::exists(path, inspect_error);
        remove_staging_directory(staging);
        if (!inspect_error && destination_exists) {
            return {{StorageError::path_exists,
                     "refusing to overwrite an existing project path"},
                    nullptr};
        }
        return {{StorageError::filesystem_failure,
                 "could not atomically publish the staged project without overwrite: " +
                     publish_error.message()},
                nullptr};
    }

    auto published = ProjectStore::open(path);
    if (!published.ok()) {
        std::error_code equivalent_error;
        const bool same_file = std::filesystem::equivalent(staged_path, path, equivalent_error);
        if (!equivalent_error && same_file) {
            std::error_code ignored;
            (void)std::filesystem::remove(path, ignored);
        }
        remove_staging_directory(staging);
        return published;
    }

    remove_staging_directory(staging);
    return published;
}

ProjectStoreResult ProjectStore::open(const std::filesystem::path& path) {
    if (path.empty()) {
        return {{StorageError::invalid_argument, "project path is empty"}, nullptr};
    }
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return {{StorageError::file_not_found, "project file does not exist"}, nullptr};
    }
    if (ec) {
        return {{StorageError::invalid_argument,
                 "could not inspect project path: " + ec.message()},
                nullptr};
    }

    auto impl = std::make_unique<Impl>();
    impl->path = path;
    Status status = detail::open_database(path, SQLITE_OPEN_READWRITE, impl->db);
    if (!status) return {std::move(status), nullptr};

    if (!(status = detail::verify_quick_check(impl->db.get()))) return {std::move(status), nullptr};
    if (!(status = validate_identity(impl->db.get()))) return {std::move(status), nullptr};
    if (!(status = load_metadata(impl->db.get(), impl->metadata))) return {std::move(status), nullptr};
    if (!(status = validate_schema_surface(impl->db.get()))) return {std::move(status), nullptr};
    if (!(status = validate_frozen_claim(impl->db.get(), impl->metadata))) return {std::move(status), nullptr};
    if (!(status = detail::configure_durable(impl->db.get()))) return {std::move(status), nullptr};

    return {Status::success(), std::unique_ptr<ProjectStore>(new ProjectStore(std::move(impl)))};
}

const ProjectMetadata& ProjectStore::metadata() const noexcept { return impl_->metadata; }
const std::filesystem::path& ProjectStore::path() const noexcept { return impl_->path; }

Status ProjectStore::refresh_metadata() {
    ProjectMetadata current;
    Status status = load_metadata(impl_->db.get(), current);
    if (!status) return status;
    if (!impl_->metadata.project_uuid.empty() && current.project_uuid != impl_->metadata.project_uuid) {
        return {StorageError::schema_invalid,
                "project UUID changed while the project handle was open"};
    }
    if (!(status = validate_frozen_claim(impl_->db.get(), current))) return status;
    impl_->metadata = std::move(current);
    return Status::success();
}

Status ProjectStore::update_metadata(const ProjectMetadataUpdate& update) {
    Status status = validate_metadata_update(update);
    if (!status) return status;
    if (!(status = detail::begin_immediate(impl_->db.get()))) return status;

    ProjectMetadata current;
    status = load_metadata(impl_->db.get(), current);
    if (!status || current.project_uuid != impl_->metadata.project_uuid) {
        detail::rollback(impl_->db.get());
        if (!status) return status;
        return {StorageError::schema_invalid,
                "project UUID changed while applying a mutation"};
    }
    if (!(status = validate_frozen_claim(impl_->db.get(), current))) {
        detail::rollback(impl_->db.get());
        return status;
    }
    if (current.revision >= static_cast<std::uint64_t>(std::numeric_limits<sqlite3_int64>::max())) {
        detail::rollback(impl_->db.get());
        return {StorageError::schema_invalid,
                "project revision exhausted signed SQLite integer range"};
    }

    const std::string projection = update.projection_id.value_or(current.projection_id);
    const std::string worldview = update.worldview_id.value_or(current.worldview_id);
    const std::uint64_t next_revision = current.revision + 1U;

    detail::StmtPtr stmt;
    status = detail::prepare(
        impl_->db.get(),
        "UPDATE aeris_meta SET revision=?,modified_utc=?,projection_id=?,worldview_id=? "
        "WHERE id=1 AND project_uuid=?;",
        stmt);
    if (status) {
        status = detail::bind_int64(
            impl_->db.get(), stmt.get(), 1, static_cast<sqlite3_int64>(next_revision));
    }
    if (status) status = detail::bind_text(impl_->db.get(), stmt.get(), 2, update.modified_utc);
    if (status) status = detail::bind_text(impl_->db.get(), stmt.get(), 3, projection);
    if (status) status = detail::bind_text(impl_->db.get(), stmt.get(), 4, worldview);
    if (status) status = detail::bind_text(impl_->db.get(), stmt.get(), 5, current.project_uuid);
    if (status) status = detail::step_done(impl_->db.get(), stmt.get());
    if (status && sqlite3_changes(impl_->db.get()) != 1) {
        status = {StorageError::schema_invalid,
                  "aeris_meta singleton update did not affect exactly one row"};
    }
    if (!status) {
        detail::rollback(impl_->db.get());
        return status;
    }
    status = detail::commit(impl_->db.get());
    if (!status) {
        detail::rollback(impl_->db.get());
        return status;
    }

    current.revision = next_revision;
    current.modified_utc = update.modified_utc;
    current.projection_id = projection;
    current.worldview_id = worldview;
    impl_->metadata = std::move(current);
    return Status::success();
}

Status ProjectStore::verify_integrity() const {
    Status status = detail::verify_quick_check(impl_->db.get());
    if (!status) return status;
    if (!(status = validate_identity(impl_->db.get()))) return status;
    ProjectMetadata metadata;
    if (!(status = load_metadata(impl_->db.get(), metadata))) return status;
    if (!(status = validate_schema_surface(impl_->db.get()))) return status;
    if (!(status = validate_frozen_claim(impl_->db.get(), metadata))) return status;
    if (!(status = detail::verify_geometry_semantics(*this))) return status;
    if (!(status = detail::verify_resource_semantics(*this))) return status;
    return detail::verify_layer_semantics(*this);
}

}  // namespace aeris::storage
