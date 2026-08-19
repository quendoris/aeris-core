// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#include "sqlite_detail.hpp"

#include <sstream>
#include <utility>

namespace aeris::storage::detail {
namespace {

Status install_source_materialization_guards(sqlite3* db) {
    sqlite3_stmt* raw = nullptr;
    const int prepare_rc = sqlite3_prepare_v2(
        db,
        "SELECT 1 FROM sqlite_schema WHERE type='table' AND name='aeris_source' LIMIT 1;",
        -1,
        &raw,
        nullptr);
    if (prepare_rc != SQLITE_OK) {
        return {StorageError::sqlite_failure,
                sqlite_message(db, "could not inspect source schema before installing guards")};
    }

    const int step_rc = sqlite3_step(raw);
    (void)sqlite3_finalize(raw);
    if (step_rc == SQLITE_DONE) {
        // Project creation configures durability before the schema exists. The
        // published project is reopened immediately after schema creation, at
        // which point these persistent triggers are installed.
        return Status::success();
    }
    if (step_rc != SQLITE_ROW) {
        return {StorageError::sqlite_failure,
                sqlite_message(db, "could not inspect source schema before installing guards")};
    }

    return exec(
        db,
        "CREATE TRIGGER IF NOT EXISTS aeris_guard_source_materialization_monotonic "
        "BEFORE UPDATE OF materialization_state ON aeris_source "
        "WHEN OLD.materialization_state=1 AND NEW.materialization_state<>1 "
        "BEGIN SELECT RAISE(ABORT,'AERIS materialized source cannot be demoted'); END;"

        "CREATE TRIGGER IF NOT EXISTS aeris_guard_freeze_referenced_source "
        "BEFORE UPDATE OF frozen ON aeris_meta "
        "WHEN NEW.frozen=1 AND EXISTS("
        "SELECT 1 FROM aeris_layer_source ls "
        "JOIN aeris_source s ON s.source_id=ls.source_id "
        "WHERE s.materialization_state<>1) "
        "BEGIN SELECT RAISE(ABORT,'AERIS frozen project cannot bind referenced source'); END;"

        "CREATE TRIGGER IF NOT EXISTS aeris_guard_frozen_layer_source_insert "
        "BEFORE INSERT ON aeris_layer_source "
        "WHEN EXISTS(SELECT 1 FROM aeris_meta WHERE id=1 AND frozen=1) "
        "AND EXISTS(SELECT 1 FROM aeris_source s "
        "WHERE s.source_id=NEW.source_id AND s.materialization_state<>1) "
        "BEGIN SELECT RAISE(ABORT,'AERIS frozen project cannot bind referenced source'); END;"

        "CREATE TRIGGER IF NOT EXISTS aeris_guard_frozen_layer_source_update "
        "BEFORE UPDATE OF source_id ON aeris_layer_source "
        "WHEN EXISTS(SELECT 1 FROM aeris_meta WHERE id=1 AND frozen=1) "
        "AND EXISTS(SELECT 1 FROM aeris_source s "
        "WHERE s.source_id=NEW.source_id AND s.materialization_state<>1) "
        "BEGIN SELECT RAISE(ABORT,'AERIS frozen project cannot bind referenced source'); END;"

        "CREATE TRIGGER IF NOT EXISTS aeris_guard_frozen_bound_source_demotion "
        "BEFORE UPDATE OF materialization_state ON aeris_source "
        "WHEN NEW.materialization_state<>1 "
        "AND EXISTS(SELECT 1 FROM aeris_meta WHERE id=1 AND frozen=1) "
        "AND EXISTS(SELECT 1 FROM aeris_layer_source ls WHERE ls.source_id=OLD.source_id) "
        "BEGIN SELECT RAISE(ABORT,'AERIS frozen project cannot demote bound source'); END;"
    );
}

}  // namespace

void DbCloser::operator()(sqlite3* db) const noexcept {
    if (db != nullptr) {
        (void)sqlite3_close_v2(db);
    }
}

void StmtFinalizer::operator()(sqlite3_stmt* stmt) const noexcept {
    if (stmt != nullptr) {
        (void)sqlite3_finalize(stmt);
    }
}

std::string sqlite_message(sqlite3* db, const char* context) {
    std::ostringstream stream;
    stream << context;
    if (db != nullptr) {
        stream << ": " << sqlite3_errmsg(db) << " (" << sqlite3_extended_errcode(db) << ')';
    }
    return stream.str();
}

Status open_database(const std::filesystem::path& path, const int flags, DbPtr& out) {
    sqlite3* raw = nullptr;
    const std::string native = path.u8string();
    const int rc = sqlite3_open_v2(native.c_str(), &raw, flags, nullptr);
    if (rc != SQLITE_OK) {
        const std::string diagnostic = raw != nullptr ? sqlite_message(raw, "sqlite open failed") : "sqlite open failed";
        if (raw != nullptr) {
            (void)sqlite3_close_v2(raw);
        }
        return {StorageError::sqlite_open_failed, diagnostic};
    }
    sqlite3_extended_result_codes(raw, 1);
    sqlite3_busy_timeout(raw, 5000);
    out.reset(raw);
    return Status::success();
}

Status exec(sqlite3* db, const char* sql) {
    char* error = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &error);
    if (rc != SQLITE_OK) {
        std::string diagnostic = "sqlite exec failed";
        if (error != nullptr) {
            diagnostic += ": ";
            diagnostic += error;
            sqlite3_free(error);
        } else {
            diagnostic = sqlite_message(db, diagnostic.c_str());
        }
        return {StorageError::sqlite_failure, std::move(diagnostic)};
    }
    return Status::success();
}

Status configure_durable(sqlite3* db) {
    // DELETE + EXTRA is deliberate for acknowledged-mutation durability. SQLite
    // documents EXTRA as adding the directory sync after rollback-journal unlink.
    Status status = exec(db, "PRAGMA foreign_keys=ON;");
    if (!status) return status;
    status = exec(db, "PRAGMA journal_mode=DELETE;");
    if (!status) return status;
    status = exec(db, "PRAGMA synchronous=EXTRA;");
    if (!status) return status;
    status = exec(db, "PRAGMA temp_store=MEMORY;");
    if (!status) return status;
    return install_source_materialization_guards(db);
}

Status prepare(sqlite3* db, const char* sql, StmtPtr& out) {
    sqlite3_stmt* raw = nullptr;
    const int rc = sqlite3_prepare_v2(db, sql, -1, &raw, nullptr);
    if (rc != SQLITE_OK) {
        return {StorageError::sqlite_failure, sqlite_message(db, "sqlite prepare failed")};
    }
    out.reset(raw);
    return Status::success();
}

Status bind_text(sqlite3* db, sqlite3_stmt* stmt, const int index, const std::string& value) {
    const int rc = sqlite3_bind_text(stmt, index, value.c_str(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
    if (rc != SQLITE_OK) {
        return {StorageError::sqlite_failure, sqlite_message(db, "sqlite bind text failed")};
    }
    return Status::success();
}

Status bind_int64(sqlite3* db, sqlite3_stmt* stmt, const int index, const sqlite3_int64 value) {
    const int rc = sqlite3_bind_int64(stmt, index, value);
    if (rc != SQLITE_OK) {
        return {StorageError::sqlite_failure, sqlite_message(db, "sqlite bind int64 failed")};
    }
    return Status::success();
}

Status bind_double(sqlite3* db, sqlite3_stmt* stmt, const int index, const double value) {
    const int rc = sqlite3_bind_double(stmt, index, value);
    if (rc != SQLITE_OK) {
        return {StorageError::sqlite_failure, sqlite_message(db, "sqlite bind double failed")};
    }
    return Status::success();
}

Status step_done(sqlite3* db, sqlite3_stmt* stmt) {
    const int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        return {StorageError::sqlite_failure, sqlite_message(db, "sqlite step failed")};
    }
    return Status::success();
}

Status query_single_int(sqlite3* db, const char* sql, sqlite3_int64& value) {
    StmtPtr stmt;
    Status status = prepare(db, sql, stmt);
    if (!status) return status;
    const int rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_ROW || sqlite3_column_count(stmt.get()) < 1) {
        return {StorageError::schema_invalid, "expected exactly one scalar integer row"};
    }
    value = sqlite3_column_int64(stmt.get(), 0);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        return {StorageError::schema_invalid, "scalar integer query returned more than one row"};
    }
    return Status::success();
}

Status query_single_text(sqlite3* db, const char* sql, std::string& value) {
    StmtPtr stmt;
    Status status = prepare(db, sql, stmt);
    if (!status) return status;
    const int rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_ROW || sqlite3_column_count(stmt.get()) < 1 || sqlite3_column_type(stmt.get(), 0) == SQLITE_NULL) {
        return {StorageError::schema_invalid, "expected exactly one scalar text row"};
    }
    const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
    const int bytes = sqlite3_column_bytes(stmt.get(), 0);
    value.assign(text, static_cast<std::size_t>(bytes));
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        return {StorageError::schema_invalid, "scalar text query returned more than one row"};
    }
    return Status::success();
}

Status verify_quick_check(sqlite3* db) {
    std::string result;
    Status status = query_single_text(db, "PRAGMA quick_check(1);", result);
    if (!status) return status;
    if (result != "ok") {
        return {StorageError::integrity_failed, "SQLite quick_check failed: " + result};
    }
    return Status::success();
}

Status begin_immediate(sqlite3* db) { return exec(db, "BEGIN IMMEDIATE;"); }
Status commit(sqlite3* db) { return exec(db, "COMMIT;"); }
void rollback(sqlite3* db) noexcept { (void)sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr); }

}  // namespace aeris::storage::detail
