// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace aeris::source {

enum class DbfError : std::uint8_t {
    none = 0U,
    io_error,
    truncated_file,
    unsupported_version,
    invalid_header,
    invalid_field,
    duplicate_field,
    record_length_mismatch,
    file_length_mismatch,
    malformed_record,
};

struct DbfField final {
    std::string name;
    char type = '\0';
    std::uint8_t width = 0U;
    std::uint8_t decimal_count = 0U;
};

struct DbfRecord final {
    // Physical one-based row number in the DBF. Provider adapters use this to
    // prove alignment with companion geometry records; deleted rows are never
    // silently removed by the format reader.
    std::uint32_t record_number = 0U;
    bool deleted = false;
    std::vector<std::string> values;
};

struct DbfTableResult final {
    std::vector<DbfField> fields;
    std::vector<DbfRecord> records;
    DbfError error = DbfError::none;
    std::uint32_t failed_record_number = 0U;
    std::string diagnostic;

    [[nodiscard]] bool ok() const noexcept { return error == DbfError::none; }
};

// Strict structural reader for classic dBASE-style DBF tables used alongside
// shapefiles. It deliberately returns fixed-width field bytes without guessing
// text encoding or semantic numeric types. Provider adapters own trimming,
// encoding validation, and typed interpretation.
[[nodiscard]] DbfTableResult read_dbf_table(const std::filesystem::path& path);

}  // namespace aeris::source
