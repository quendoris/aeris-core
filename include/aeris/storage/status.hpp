// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <string>

namespace aeris::storage {

enum class StorageError {
    none,
    invalid_argument,
    path_exists,
    file_not_found,
    filesystem_failure,
    sqlite_open_failed,
    sqlite_failure,
    invalid_application_id,
    unsupported_schema,
    schema_invalid,
    invalid_project_uuid,
    session_project_mismatch,
    integrity_failed,
    record_exists,
    record_not_found,
};

struct Status {
    StorageError error{StorageError::none};
    std::string diagnostic;

    [[nodiscard]] bool ok() const noexcept { return error == StorageError::none; }
    explicit operator bool() const noexcept { return ok(); }

    static Status success() { return {}; }
};

}  // namespace aeris::storage
