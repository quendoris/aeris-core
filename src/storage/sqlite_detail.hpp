// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "aeris/storage/status.hpp"

#include <filesystem>
#include <memory>
#include <string>

#include <sqlite3.h>

namespace aeris::storage::detail {

struct DbCloser {
    void operator()(sqlite3* db) const noexcept;
};
using DbPtr = std::unique_ptr<sqlite3, DbCloser>;

struct StmtFinalizer {
    void operator()(sqlite3_stmt* stmt) const noexcept;
};
using StmtPtr = std::unique_ptr<sqlite3_stmt, StmtFinalizer>;

Status open_database(const std::filesystem::path& path, int flags, DbPtr& out);
Status configure_durable(sqlite3* db);
Status exec(sqlite3* db, const char* sql);
Status prepare(sqlite3* db, const char* sql, StmtPtr& out);
inline Status prepare(const DbPtr& db, const char* sql, StmtPtr& out) {
    return prepare(db.get(), sql, out);
}
Status bind_text(sqlite3* db, sqlite3_stmt* stmt, int index, const std::string& value);
Status bind_int64(sqlite3* db, sqlite3_stmt* stmt, int index, sqlite3_int64 value);
Status bind_double(sqlite3* db, sqlite3_stmt* stmt, int index, double value);
Status step_done(sqlite3* db, sqlite3_stmt* stmt);
Status query_single_int(sqlite3* db, const char* sql, sqlite3_int64& value);
Status query_single_text(sqlite3* db, const char* sql, std::string& value);
Status verify_quick_check(sqlite3* db);
Status begin_immediate(sqlite3* db);
Status commit(sqlite3* db);
void rollback(sqlite3* db) noexcept;
std::string sqlite_message(sqlite3* db, const char* context);

}  // namespace aeris::storage::detail
