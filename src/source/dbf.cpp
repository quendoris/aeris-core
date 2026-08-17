// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/source/dbf.hpp"

#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace aeris::source {
namespace {

constexpr std::uint32_t kMaxRecords = 1'000'000U;
constexpr std::size_t kFileHeaderBytes = 32U;
constexpr std::size_t kFieldDescriptorBytes = 32U;
constexpr unsigned char kFieldTerminator = 0x0dU;
constexpr unsigned char kFileTerminator = 0x1aU;

[[nodiscard]] std::uint16_t read_le16(const unsigned char* bytes) noexcept {
    return static_cast<std::uint16_t>(bytes[0]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[1]) << 8U);
}

[[nodiscard]] std::uint32_t read_le32(const unsigned char* bytes) noexcept {
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[2]) << 16U) |
           (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

[[nodiscard]] bool read_exact(
    std::ifstream& input,
    void* destination,
    const std::size_t bytes
) {
    if (bytes > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
        return false;
    }
    input.read(static_cast<char*>(destination), static_cast<std::streamsize>(bytes));
    return input.good() ||
           (input.eof() && input.gcount() == static_cast<std::streamsize>(bytes));
}

[[nodiscard]] bool supported_version(const unsigned char version) noexcept {
    // 0x03 is the classic dBASE III table emitted for the pinned Natural Earth
    // shapefiles. Memo-bearing and later dialects need an explicit contract
    // rather than being accepted accidentally.
    return version == 0x03U;
}

[[nodiscard]] bool supported_field_type(const char type) noexcept {
    switch (type) {
        case 'C': // character
        case 'N': // numeric text
        case 'F': // floating-point text
        case 'L': // logical
        case 'D': // date
        case 'I': // integer (later writers occasionally emit this)
        case 'B': // binary double
        case 'Y': // currency
        case 'T': // datetime
        case '@': // timestamp alias
        case '+': // autoincrement
            return true;
        default:
            return false;
    }
}

[[nodiscard]] bool valid_field_name_char(const unsigned char value) noexcept {
    return value == static_cast<unsigned char>('_') ||
           (value >= static_cast<unsigned char>('0') && value <= static_cast<unsigned char>('9')) ||
           (value >= static_cast<unsigned char>('A') && value <= static_cast<unsigned char>('Z')) ||
           (value >= static_cast<unsigned char>('a') && value <= static_cast<unsigned char>('z'));
}

[[nodiscard]] bool decode_field_name(
    const unsigned char* bytes,
    std::string& name
) {
    name.clear();
    bool padding = false;
    for (std::size_t index = 0U; index < 11U; ++index) {
        const unsigned char value = bytes[index];
        if (value == 0U) {
            padding = true;
            continue;
        }
        if (padding || !valid_field_name_char(value)) {
            return false;
        }
        name.push_back(static_cast<char>(value));
    }
    if (name.empty()) return false;
    const unsigned char first = static_cast<unsigned char>(name.front());
    return first == static_cast<unsigned char>('_') ||
           (first >= static_cast<unsigned char>('A') && first <= static_cast<unsigned char>('Z')) ||
           (first >= static_cast<unsigned char>('a') && first <= static_cast<unsigned char>('z'));
}

[[nodiscard]] DbfTableResult failure(
    const DbfError error,
    std::string diagnostic,
    const std::uint32_t record_number = 0U
) {
    DbfTableResult result{};
    result.error = error;
    result.failed_record_number = record_number;
    result.diagnostic = std::move(diagnostic);
    return result;
}

}  // namespace

DbfTableResult read_dbf_table(const std::filesystem::path& path) {
    std::error_code size_error;
    const std::uintmax_t file_size = std::filesystem::file_size(path, size_error);
    if (size_error) {
        return failure(DbfError::io_error, "could not inspect DBF file size: " + size_error.message());
    }
    if (file_size < kFileHeaderBytes + 1U) {
        return failure(DbfError::truncated_file, "DBF is shorter than the fixed header and field terminator");
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return failure(DbfError::io_error, "could not open DBF file");
    }

    std::array<unsigned char, kFileHeaderBytes> header{};
    if (!read_exact(input, header.data(), header.size())) {
        return failure(DbfError::truncated_file, "DBF fixed header is truncated");
    }
    if (!supported_version(header[0])) {
        return failure(DbfError::unsupported_version, "DBF uses an unsupported dBASE version byte");
    }

    const std::uint32_t record_count = read_le32(header.data() + 4U);
    const std::uint16_t header_bytes = read_le16(header.data() + 8U);
    const std::uint16_t record_bytes = read_le16(header.data() + 10U);
    if (record_count > kMaxRecords) {
        return failure(DbfError::invalid_header, "DBF exceeds the 1000000-record source bound");
    }
    if (header_bytes < kFileHeaderBytes + 1U ||
        (static_cast<std::size_t>(header_bytes) - (kFileHeaderBytes + 1U)) %
                kFieldDescriptorBytes != 0U) {
        return failure(DbfError::invalid_header, "DBF header length does not describe whole field descriptors plus 0x0D terminator");
    }
    if (record_bytes == 0U) {
        return failure(DbfError::invalid_header, "DBF record length is zero");
    }

    const std::size_t field_count =
        (static_cast<std::size_t>(header_bytes) - (kFileHeaderBytes + 1U)) /
        kFieldDescriptorBytes;
    if (field_count == 0U || field_count > 4096U) {
        return failure(DbfError::invalid_header, "DBF field count is outside the draft 1..4096 bound");
    }

    const std::uintmax_t rows_bytes =
        static_cast<std::uintmax_t>(record_count) * static_cast<std::uintmax_t>(record_bytes);
    const std::uintmax_t expected_size = static_cast<std::uintmax_t>(header_bytes) + rows_bytes;
    if (file_size != expected_size && file_size != expected_size + 1U) {
        return failure(DbfError::file_length_mismatch, "DBF file length disagrees with header and fixed record dimensions");
    }

    DbfTableResult result{};
    result.fields.reserve(field_count);
    std::set<std::string> names;
    std::size_t payload_width = 0U;

    for (std::size_t field_index = 0U; field_index < field_count; ++field_index) {
        std::array<unsigned char, kFieldDescriptorBytes> descriptor{};
        if (!read_exact(input, descriptor.data(), descriptor.size())) {
            return failure(DbfError::truncated_file, "DBF field descriptor is truncated");
        }

        DbfField field{};
        if (!decode_field_name(descriptor.data(), field.name)) {
            return failure(DbfError::invalid_field, "DBF field name is empty, malformed, or not canonically NUL-padded ASCII");
        }
        field.type = static_cast<char>(descriptor[11]);
        field.width = descriptor[16];
        field.decimal_count = descriptor[17];
        if (!supported_field_type(field.type)) {
            return failure(DbfError::invalid_field, "DBF field uses an unsupported type code");
        }
        if (field.width == 0U || field.decimal_count > field.width) {
            return failure(DbfError::invalid_field, "DBF field width/decimal metadata is invalid");
        }
        if (!names.insert(field.name).second) {
            return failure(DbfError::duplicate_field, "DBF contains a duplicate field name");
        }
        payload_width += static_cast<std::size_t>(field.width);
        if (payload_width + 1U > static_cast<std::size_t>(record_bytes)) {
            return failure(DbfError::record_length_mismatch, "DBF field widths exceed the fixed record length");
        }
        result.fields.push_back(std::move(field));
    }

    unsigned char terminator = 0U;
    if (!read_exact(input, &terminator, 1U)) {
        return failure(DbfError::truncated_file, "DBF header terminator is missing");
    }
    if (terminator != kFieldTerminator) {
        return failure(DbfError::invalid_header, "DBF field descriptor list is not terminated by 0x0D");
    }
    if (payload_width + 1U != static_cast<std::size_t>(record_bytes)) {
        return failure(DbfError::record_length_mismatch, "DBF record length is not exactly deletion flag plus declared field widths");
    }

    result.records.reserve(record_count);
    std::vector<char> record_buffer(static_cast<std::size_t>(record_bytes));
    for (std::uint32_t record_index = 0U; record_index < record_count; ++record_index) {
        const std::uint32_t record_number = record_index + 1U;
        if (!read_exact(input, record_buffer.data(), record_buffer.size())) {
            return failure(DbfError::truncated_file, "DBF record bytes are truncated", record_number);
        }

        const char deletion_flag = record_buffer.front();
        if (deletion_flag != ' ' && deletion_flag != '*') {
            return failure(DbfError::malformed_record, "DBF record has an invalid deletion flag", record_number);
        }

        DbfRecord record{};
        record.record_number = record_number;
        record.deleted = deletion_flag == '*';
        record.values.reserve(result.fields.size());
        std::size_t offset = 1U;
        for (const DbfField& field : result.fields) {
            const std::size_t width = static_cast<std::size_t>(field.width);
            record.values.emplace_back(record_buffer.data() + offset, width);
            offset += width;
        }
        result.records.push_back(std::move(record));
    }

    if (file_size == expected_size + 1U) {
        unsigned char eof_marker = 0U;
        if (!read_exact(input, &eof_marker, 1U) || eof_marker != kFileTerminator) {
            return failure(DbfError::file_length_mismatch, "DBF trailing byte is not the optional 0x1A end-of-file marker");
        }
    }

    char unexpected = '\0';
    input.read(&unexpected, 1);
    if (input.gcount() != 0) {
        return failure(DbfError::file_length_mismatch, "DBF contains bytes beyond the accounted table payload");
    }

    return result;
}

}  // namespace aeris::source
