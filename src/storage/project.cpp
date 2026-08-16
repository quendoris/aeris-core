// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#include "aeris/storage/project.hpp"

#include "sqlite_detail.hpp"

#include <array>
#include <limits>
#include <utility>

#include <sqlite3.h>

namespace aeris::storage {
namespace {

constexpr std::size_t kMaxMetadataText = 255U;

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

Status validate_create_options(const ProjectCreateOptions& options) {
    if (!is_canonical_utc_timestamp(options.timestamp_utc)) {
        return {StorageError::invalid_argument, "project creation timestamp must be canonical UTC YYYY-MM-DDTHH:MM:SSZ"};
    }
    if (!options.project_uuid.empty() && !is_canonical_uuid(options.project_uuid)) {
        return {StorageError::invalid_project_uuid, "project UUID is not canonical 8-4-4-4-12 hexadecimal form"};
    }
    if (!valid_small_text(options.producer) || !valid_small_text(options.producer_version) ||
        !valid_small_text(options.projection_id) || !valid_small_text(options.worldview_id)) {
        return {StorageError::invalid_argument, "project metadata identifiers must be non-empty, NUL-free, and at most 255 bytes"};
    }
    return Status::success();
}

Status validate_metadata_update(const ProjectMetadataUpdate& update) {
    if (!is_canonical_utc_timestamp(update.modified_utc)) {
        return {StorageError::invalid_argument, "project mutation timestamp must be canonical UTC YYYY-MM-DDTHH:MM:SSZ"};
    }
    if (update.projection_id && !valid_small_text(*update.projection_id)) {
        return {StorageError::invalid_argument, "projection identifier must be non-empty, NUL-free, and at most 255 bytes"};
    }
    if (update.worldview_id && !valid_small_text(*update.worldview_id)) {
        return {StorageError::invalid_argument, "worldview identifier must be non-empty, NUL-free, and at most 255 bytes"};
    }
    if (!update.projection_id && !update.worldview_id && !update.frozen) {
        return {StorageError::invalid_argument, "metadata update contains no acknowledged project mutation"};
    }
    return Status::success();
}

Status validate_identity(sqlite3* db) {
    sqlite3_int64 application_id = 0;
    Status status = detail::query_single_int(db, "PRAGMA application_id;", application_id);
    if (!status) return status;
    if (application_id != static_cast<sqlite3_int64>(kProjectApplicationId)) {
        return {StorageError::invalid_application_id, "file is not an AERIS project (application_id mismatch)"};
    }

    sqlite3_int64 schema_generation = 0;
    status = detail::query_single_int(db, "PRAGMA user_version;", schema_generation);
    if (!status) return status;
    if (schema_generation != kProjectSchemaGeneration) {
        return {StorageError::unsupported_schema, "unsupported AERIS draft project schema generation"};
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
        return {StorageError::schema_invalid, "aeris_meta singleton row is missing"};
    }
    for (int column : {0, 4, 5, 6, 7, 8, 9}) {
        if (sqlite3_column_type(stmt.get(), column) != SQLITE_TEXT) {
            return {StorageError::schema_invalid, "aeris_meta contains a non-text value in a required text column"};
        }
    }
    for (int column : {1, 2, 3, 10}) {
        if (sqlite3_column_type(stmt.get(), column) != SQLITE_INTEGER) {
            return {StorageError::schema_invalid, "aeris_meta contains a non-integer value in a required integer column"};
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
        return {StorageError::schema_invalid, "aeris_meta contains more than the singleton row"};
    }
    if (!is_canonical_uuid(metadata.project_uuid)) {
        return {StorageError::invalid_project_uuid, "stored project UUID is invalid"};
    }
    if (metadata.format_major != kDraftFormatMajor || metadata.format_minor != kDraftFormatMinor) {
        return {StorageError::unsupported_schema, "unsupported AERIS draft format version"};
    }
    if (revision < 0) {
        return {StorageError::schema_invalid, "project revision is negative"};
    }
    if (!is_canonical_utc_timestamp(metadata.created_utc) || !is_canonical_utc_timestamp(metadata.modified_utc)) {
        return {StorageError::schema_invalid, "project timestamps are not canonical UTC values"};
    }
    if (!valid_small_text(metadata.producer) || !valid_small_text(metadata.producer_version) ||
        !valid_small_text(metadata.projection_id) || !valid_small_text(metadata.worldview_id)) {
        return {StorageError::schema_invalid, "project metadata identifier violates storage bounds"};
    }
    if (frozen != 0 && frozen != 1) {
        return {StorageError::schema_invalid, "project frozen flag is not boolean"};
    }
    metadata.revision = static_cast<std::uint64_t>(revision);
    metadata.frozen = frozen == 1;
    return Status::success();
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
    const int month = two_digits(value, 5U);
    const int day = two_digits(value, 8U);
    const int hour = two_digits(value, 11U);
    const int minute = two_digits(value, 14U);
    const int second = two_digits(value, 17U);
    return month >= 1 && month <= 12 && day >= 1 && day <= 31 && hour >= 0 && hour <= 23 &&
           minute >= 0 && minute <= 59 && second >= 0 && second <= 59;
}

ProjectStore::ProjectStore(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
ProjectStore::~ProjectStore() = default;
ProjectStore::ProjectStore(ProjectStore&&) noexcept = default;
ProjectStore& ProjectStore::operator=(ProjectStore&&) noexcept = default;

ProjectStoreResult ProjectStore::create(const std::filesystem::path& path, const ProjectCreateOptions& options) {
    if (path.empty()) return {{StorageError::invalid_argument, "project path is empty"}, nullptr};
    Status status = validate_create_options(options);
    if (!status) return {std::move(status), nullptr};

    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        return {{StorageError::path_exists, "refusing to overwrite an existing project path"}, nullptr};
    }
    if (ec) return {{StorageError::invalid_argument, "could not inspect project path: " + ec.message()}, nullptr};

    auto impl = std::make_unique<Impl>();
    impl->path = path;
    status = detail::open_database(path, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, impl->db);
    if (!status) return {std::move(status), nullptr};

    const auto cleanup_failed_create = [&]() {
        impl->db.reset();
        std::error_code ignored;
        (void)std::filesystem::remove(path, ignored);
    };

    if (!(status = detail::configure_durable(impl->db.get()))) {
        cleanup_failed_create();
        return {std::move(status), nullptr};
    }
    if (!(status = detail::begin_immediate(impl->db.get()))) {
        cleanup_failed_create();
        return {std::move(status), nullptr};
    }

    const char* schema =
        "PRAGMA application_id=1095062089;"
        "PRAGMA user_version=1;"
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
        ");";
    status = detail::exec(impl->db.get(), schema);
    if (!status) {
        detail::rollback(impl->db.get());
        cleanup_failed_create();
        return {std::move(status), nullptr};
    }

    impl->metadata.project_uuid = options.project_uuid.empty() ? generated_uuid_v4() : options.project_uuid;
    impl->metadata.created_utc = options.timestamp_utc;
    impl->metadata.modified_utc = options.timestamp_utc;
    impl->metadata.producer = options.producer;
    impl->metadata.producer_version = options.producer_version;
    impl->metadata.projection_id = options.projection_id;
    impl->metadata.worldview_id = options.worldview_id;
    impl->metadata.frozen = options.frozen;

    status = insert_metadata(impl->db.get(), impl->metadata);
    if (!status) {
        detail::rollback(impl->db.get());
        cleanup_failed_create();
        return {std::move(status), nullptr};
    }
    status = detail::commit(impl->db.get());
    if (!status) {
        detail::rollback(impl->db.get());
        cleanup_failed_create();
        return {std::move(status), nullptr};
    }
    status = detail::verify_quick_check(impl->db.get());
    if (!status) {
        cleanup_failed_create();
        return {std::move(status), nullptr};
    }
    return {Status::success(), std::unique_ptr<ProjectStore>(new ProjectStore(std::move(impl)))};
}

ProjectStoreResult ProjectStore::open(const std::filesystem::path& path) {
    if (path.empty()) return {{StorageError::invalid_argument, "project path is empty"}, nullptr};
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return {{StorageError::file_not_found, "project file does not exist"}, nullptr};
    }
    if (ec) return {{StorageError::invalid_argument, "could not inspect project path: " + ec.message()}, nullptr};

    auto impl = std::make_unique<Impl>();
    impl->path = path;
    Status status = detail::open_database(path, SQLITE_OPEN_READWRITE, impl->db);
    if (!status) return {std::move(status), nullptr};
    if (!(status = detail::configure_durable(impl->db.get()))) return {std::move(status), nullptr};
    if (!(status = detail::verify_quick_check(impl->db.get()))) return {std::move(status), nullptr};
    if (!(status = validate_identity(impl->db.get()))) return {std::move(status), nullptr};
    if (!(status = load_metadata(impl->db.get(), impl->metadata))) return {std::move(status), nullptr};
    return {Status::success(), std::unique_ptr<ProjectStore>(new ProjectStore(std::move(impl)))};
}

const ProjectMetadata& ProjectStore::metadata() const noexcept { return impl_->metadata; }
const std::filesystem::path& ProjectStore::path() const noexcept { return impl_->path; }

Status ProjectStore::update_metadata(const ProjectMetadataUpdate& update) {
    Status status = validate_metadata_update(update);
    if (!status) return status;
    if (impl_->metadata.revision >= static_cast<std::uint64_t>(std::numeric_limits<sqlite3_int64>::max())) {
        return {StorageError::schema_invalid, "project revision exhausted signed SQLite integer range"};
    }

    const std::string projection = update.projection_id.value_or(impl_->metadata.projection_id);
    const std::string worldview = update.worldview_id.value_or(impl_->metadata.worldview_id);
    const bool frozen = update.frozen.value_or(impl_->metadata.frozen);
    const std::uint64_t next_revision = impl_->metadata.revision + 1U;

    if (!(status = detail::begin_immediate(impl_->db.get()))) return status;
    detail::StmtPtr stmt;
    status = detail::prepare(
        impl_->db.get(),
        "UPDATE aeris_meta SET revision=?,modified_utc=?,projection_id=?,worldview_id=?,frozen=? WHERE id=1;",
        stmt);
    if (status) status = detail::bind_int64(impl_->db.get(), stmt.get(), 1, static_cast<sqlite3_int64>(next_revision));
    if (status) status = detail::bind_text(impl_->db.get(), stmt.get(), 2, update.modified_utc);
    if (status) status = detail::bind_text(impl_->db.get(), stmt.get(), 3, projection);
    if (status) status = detail::bind_text(impl_->db.get(), stmt.get(), 4, worldview);
    if (status) status = detail::bind_int64(impl_->db.get(), stmt.get(), 5, frozen ? 1 : 0);
    if (status) status = detail::step_done(impl_->db.get(), stmt.get());
    if (status && sqlite3_changes(impl_->db.get()) != 1) {
        status = {StorageError::schema_invalid, "aeris_meta singleton update did not affect exactly one row"};
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

    impl_->metadata.revision = next_revision;
    impl_->metadata.modified_utc = update.modified_utc;
    impl_->metadata.projection_id = projection;
    impl_->metadata.worldview_id = worldview;
    impl_->metadata.frozen = frozen;
    return Status::success();
}

Status ProjectStore::verify_integrity() const {
    Status status = detail::verify_quick_check(impl_->db.get());
    if (!status) return status;
    if (!(status = validate_identity(impl_->db.get()))) return status;
    ProjectMetadata metadata;
    return load_metadata(impl_->db.get(), metadata);
}

}  // namespace aeris::storage
