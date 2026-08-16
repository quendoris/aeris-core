// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#include "aeris/storage/session.hpp"

#include "aeris/storage/project.hpp"
#include "sqlite_detail.hpp"

#include <cmath>
#include <limits>
#include <utility>

#include <sqlite3.h>

namespace aeris::storage {
namespace {

constexpr double kHalfPi = 1.57079632679489661923132169163975144;

const char* mode_text(const SessionViewState::Mode mode) noexcept {
    switch (mode) {
        case SessionViewState::Mode::globe: return "globe";
        case SessionViewState::Mode::sinusoidal: return "sinusoidal";
        case SessionViewState::Mode::mollweide: return "mollweide";
    }
    return nullptr;
}

bool parse_mode(const std::string& value, SessionViewState::Mode& mode) noexcept {
    if (value == "globe") { mode = SessionViewState::Mode::globe; return true; }
    if (value == "sinusoidal") { mode = SessionViewState::Mode::sinusoidal; return true; }
    if (value == "mollweide") { mode = SessionViewState::Mode::mollweide; return true; }
    return false;
}

Status validate_view_state(const SessionViewState& state) {
    if (mode_text(state.mode) == nullptr || !std::isfinite(state.camera_longitude_rad) ||
        !std::isfinite(state.camera_latitude_rad) || !std::isfinite(state.zoom) ||
        state.camera_latitude_rad < -kHalfPi || state.camera_latitude_rad > kHalfPi || state.zoom <= 0.0) {
        return {StorageError::invalid_argument, "session view state contains invalid mode, camera, or zoom"};
    }
    return Status::success();
}

Status validate_identity(sqlite3* db) {
    sqlite3_int64 application_id = 0;
    Status status = detail::query_single_int(db, "PRAGMA application_id;", application_id);
    if (!status) return status;
    if (application_id != static_cast<sqlite3_int64>(kSessionApplicationId)) {
        return {StorageError::invalid_application_id, "file is not an AERIS session sidecar (application_id mismatch)"};
    }
    sqlite3_int64 schema_generation = 0;
    status = detail::query_single_int(db, "PRAGMA user_version;", schema_generation);
    if (!status) return status;
    if (schema_generation != kSessionSchemaGeneration) {
        return {StorageError::unsupported_schema, "unsupported AERIS session schema generation"};
    }
    return Status::success();
}

Status load_metadata(sqlite3* db, SessionMetadata& metadata) {
    detail::StmtPtr stmt;
    Status status = detail::prepare(
        db,
        "SELECT project_uuid,revision,modified_utc FROM aeris_session_meta WHERE id=1;",
        stmt);
    if (!status) return status;
    if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
        return {StorageError::schema_invalid, "aeris_session_meta singleton row is missing"};
    }
    if (sqlite3_column_type(stmt.get(), 0) != SQLITE_TEXT || sqlite3_column_type(stmt.get(), 1) != SQLITE_INTEGER ||
        sqlite3_column_type(stmt.get(), 2) != SQLITE_TEXT) {
        return {StorageError::schema_invalid, "aeris_session_meta contains invalid SQLite types"};
    }
    const auto* uuid_text = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
    metadata.project_uuid.assign(uuid_text, static_cast<std::size_t>(sqlite3_column_bytes(stmt.get(), 0)));
    const sqlite3_int64 revision = sqlite3_column_int64(stmt.get(), 1);
    const auto* time_text = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 2));
    metadata.modified_utc.assign(time_text, static_cast<std::size_t>(sqlite3_column_bytes(stmt.get(), 2)));
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        return {StorageError::schema_invalid, "aeris_session_meta contains more than the singleton row"};
    }
    if (!is_canonical_uuid(metadata.project_uuid)) {
        return {StorageError::invalid_project_uuid, "session sidecar stores an invalid project UUID"};
    }
    if (revision < 0) {
        return {StorageError::schema_invalid, "session revision is negative"};
    }
    if (!is_canonical_utc_timestamp(metadata.modified_utc)) {
        return {StorageError::schema_invalid, "session timestamp is not canonical UTC"};
    }
    metadata.revision = static_cast<std::uint64_t>(revision);
    return Status::success();
}

Status initialize_session(sqlite3* db, const std::string& project_uuid, const std::string& timestamp) {
    Status status = detail::begin_immediate(db);
    if (!status) return status;
    const char* schema =
        "PRAGMA application_id=1095062355;"
        "PRAGMA user_version=1;"
        "CREATE TABLE aeris_session_meta("
        "id INTEGER PRIMARY KEY CHECK(id=1),"
        "project_uuid TEXT NOT NULL,"
        "revision INTEGER NOT NULL CHECK(revision>=0),"
        "modified_utc TEXT NOT NULL"
        ");"
        "CREATE TABLE aeris_session_view("
        "id INTEGER PRIMARY KEY CHECK(id=1),"
        "mode TEXT NOT NULL CHECK(mode IN ('globe','sinusoidal','mollweide')) ,"
        "camera_longitude_rad REAL NOT NULL,"
        "camera_latitude_rad REAL NOT NULL,"
        "zoom REAL NOT NULL CHECK(zoom>0)"
        ");";
    status = detail::exec(db, schema);
    if (!status) {
        detail::rollback(db);
        return status;
    }

    detail::StmtPtr stmt;
    status = detail::prepare(db, "INSERT INTO aeris_session_meta(id,project_uuid,revision,modified_utc) VALUES(1,?,0,?);", stmt);
    if (status) status = detail::bind_text(db, stmt.get(), 1, project_uuid);
    if (status) status = detail::bind_text(db, stmt.get(), 2, timestamp);
    if (status) status = detail::step_done(db, stmt.get());
    if (!status) {
        detail::rollback(db);
        return status;
    }
    status = detail::commit(db);
    if (!status) detail::rollback(db);
    return status;
}

}  // namespace

class SessionStore::Impl {
public:
    detail::DbPtr db;
    std::filesystem::path path;
    SessionMetadata metadata;
};

std::filesystem::path adjacent_session_path(const std::filesystem::path& project_path) {
    std::filesystem::path result = project_path;
    result += ".session";
    return result;
}

SessionStore::SessionStore(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
SessionStore::~SessionStore() = default;
SessionStore::SessionStore(SessionStore&&) noexcept = default;
SessionStore& SessionStore::operator=(SessionStore&&) noexcept = default;

SessionStoreResult SessionStore::open_or_create(
    const std::filesystem::path& project_path,
    const std::string_view project_uuid,
    const std::string_view timestamp_utc) {
    if (project_path.empty() || !is_canonical_uuid(project_uuid) || !is_canonical_utc_timestamp(timestamp_utc)) {
        return {{StorageError::invalid_argument, "session requires a project path, canonical project UUID, and canonical UTC timestamp"}, nullptr};
    }

    auto impl = std::make_unique<Impl>();
    impl->path = adjacent_session_path(project_path);
    std::error_code ec;
    const bool existed = std::filesystem::exists(impl->path, ec);
    if (ec) return {{StorageError::invalid_argument, "could not inspect session path: " + ec.message()}, nullptr};

    Status status = detail::open_database(
        impl->path,
        SQLITE_OPEN_READWRITE | (existed ? 0 : SQLITE_OPEN_CREATE),
        impl->db);
    if (!status) return {std::move(status), nullptr};
    if (!(status = detail::configure_durable(impl->db.get()))) return {std::move(status), nullptr};

    if (!existed) {
        status = initialize_session(impl->db.get(), std::string(project_uuid), std::string(timestamp_utc));
        if (!status) {
            impl->db.reset();
            std::error_code ignored;
            (void)std::filesystem::remove(impl->path, ignored);
            return {std::move(status), nullptr};
        }
    }

    if (!(status = detail::verify_quick_check(impl->db.get()))) return {std::move(status), nullptr};
    if (!(status = validate_identity(impl->db.get()))) return {std::move(status), nullptr};
    if (!(status = load_metadata(impl->db.get(), impl->metadata))) return {std::move(status), nullptr};
    if (impl->metadata.project_uuid != project_uuid) {
        return {{StorageError::session_project_mismatch, "session sidecar belongs to a different project UUID and was not applied"}, nullptr};
    }
    return {Status::success(), std::unique_ptr<SessionStore>(new SessionStore(std::move(impl)))};
}

const SessionMetadata& SessionStore::metadata() const noexcept { return impl_->metadata; }
const std::filesystem::path& SessionStore::path() const noexcept { return impl_->path; }

SessionViewResult SessionStore::read_view_state() const {
    detail::StmtPtr stmt;
    Status status = detail::prepare(
        impl_->db.get(),
        "SELECT mode,camera_longitude_rad,camera_latitude_rad,zoom FROM aeris_session_view WHERE id=1;",
        stmt);
    if (!status) return {std::move(status), std::nullopt};
    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_DONE) return {Status::success(), std::nullopt};
    if (rc != SQLITE_ROW) {
        return {{StorageError::sqlite_failure, detail::sqlite_message(impl_->db.get(), "could not read session view state")}, std::nullopt};
    }
    if (sqlite3_column_type(stmt.get(), 0) != SQLITE_TEXT || sqlite3_column_type(stmt.get(), 1) != SQLITE_FLOAT ||
        sqlite3_column_type(stmt.get(), 2) != SQLITE_FLOAT || sqlite3_column_type(stmt.get(), 3) != SQLITE_FLOAT) {
        return {{StorageError::schema_invalid, "session view row contains invalid SQLite types"}, std::nullopt};
    }
    const auto* mode_value = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
    const int mode_bytes = sqlite3_column_bytes(stmt.get(), 0);
    std::string mode(mode_value, static_cast<std::size_t>(mode_bytes));
    SessionViewState state;
    if (!parse_mode(mode, state.mode)) {
        return {{StorageError::schema_invalid, "session view mode is unknown"}, std::nullopt};
    }
    state.camera_longitude_rad = sqlite3_column_double(stmt.get(), 1);
    state.camera_latitude_rad = sqlite3_column_double(stmt.get(), 2);
    state.zoom = sqlite3_column_double(stmt.get(), 3);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        return {{StorageError::schema_invalid, "session view table returned more than one singleton row"}, std::nullopt};
    }
    status = validate_view_state(state);
    if (!status) return {{StorageError::schema_invalid, status.diagnostic}, std::nullopt};
    return {Status::success(), state};
}

Status SessionStore::write_view_state(const SessionViewState& state, const std::string_view modified_utc) {
    Status status = validate_view_state(state);
    if (!status) return status;
    if (!is_canonical_utc_timestamp(modified_utc)) {
        return {StorageError::invalid_argument, "session mutation timestamp must be canonical UTC YYYY-MM-DDTHH:MM:SSZ"};
    }
    if (impl_->metadata.revision >= static_cast<std::uint64_t>(std::numeric_limits<sqlite3_int64>::max())) {
        return {StorageError::schema_invalid, "session revision exhausted signed SQLite integer range"};
    }
    const std::uint64_t next_revision = impl_->metadata.revision + 1U;

    if (!(status = detail::begin_immediate(impl_->db.get()))) return status;
    detail::StmtPtr stmt;
    status = detail::prepare(
        impl_->db.get(),
        "INSERT INTO aeris_session_view(id,mode,camera_longitude_rad,camera_latitude_rad,zoom) VALUES(1,?,?,?,?) "
        "ON CONFLICT(id) DO UPDATE SET mode=excluded.mode,camera_longitude_rad=excluded.camera_longitude_rad,"
        "camera_latitude_rad=excluded.camera_latitude_rad,zoom=excluded.zoom;",
        stmt);
    const char* mode = mode_text(state.mode);
    if (status) status = detail::bind_text(impl_->db.get(), stmt.get(), 1, std::string(mode));
    if (status) status = detail::bind_double(impl_->db.get(), stmt.get(), 2, state.camera_longitude_rad);
    if (status) status = detail::bind_double(impl_->db.get(), stmt.get(), 3, state.camera_latitude_rad);
    if (status) status = detail::bind_double(impl_->db.get(), stmt.get(), 4, state.zoom);
    if (status) status = detail::step_done(impl_->db.get(), stmt.get());

    detail::StmtPtr meta_stmt;
    if (status) status = detail::prepare(impl_->db.get(), "UPDATE aeris_session_meta SET revision=?,modified_utc=? WHERE id=1;", meta_stmt);
    if (status) status = detail::bind_int64(impl_->db.get(), meta_stmt.get(), 1, static_cast<sqlite3_int64>(next_revision));
    if (status) status = detail::bind_text(impl_->db.get(), meta_stmt.get(), 2, std::string(modified_utc));
    if (status) status = detail::step_done(impl_->db.get(), meta_stmt.get());
    if (status && sqlite3_changes(impl_->db.get()) != 1) {
        status = {StorageError::schema_invalid, "aeris_session_meta singleton update did not affect exactly one row"};
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
    impl_->metadata.modified_utc = std::string(modified_utc);
    return Status::success();
}

Status SessionStore::verify_integrity() const {
    Status status = detail::verify_quick_check(impl_->db.get());
    if (!status) return status;
    if (!(status = validate_identity(impl_->db.get()))) return status;
    SessionMetadata metadata;
    return load_metadata(impl_->db.get(), metadata);
}

}  // namespace aeris::storage
