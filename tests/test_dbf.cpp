// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/source/dbf.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void expect_true(const std::string_view name, const bool condition) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL " << name << '\n';
    }
}

struct FieldSpec final {
    std::string name;
    char type = 'C';
    std::uint8_t width = 0U;
    std::uint8_t decimals = 0U;
};

struct RowSpec final {
    bool deleted = false;
    std::vector<std::string> values;
};

void write_le16(std::vector<unsigned char>& bytes, const std::size_t offset, const std::uint16_t value) {
    bytes[offset] = static_cast<unsigned char>(value & 0xffU);
    bytes[offset + 1U] = static_cast<unsigned char>((value >> 8U) & 0xffU);
}

void write_le32(std::vector<unsigned char>& bytes, const std::size_t offset, const std::uint32_t value) {
    for (unsigned index = 0U; index < 4U; ++index) {
        bytes[offset + index] = static_cast<unsigned char>((value >> (8U * index)) & 0xffU);
    }
}

std::vector<unsigned char> build_dbf(
    const std::vector<FieldSpec>& fields,
    const std::vector<RowSpec>& rows,
    const bool eof_marker = true
) {
    std::size_t record_bytes = 1U;
    for (const FieldSpec& field : fields) record_bytes += field.width;
    const std::size_t header_bytes = 32U + fields.size() * 32U + 1U;

    std::vector<unsigned char> bytes(header_bytes + rows.size() * record_bytes + (eof_marker ? 1U : 0U), 0U);
    bytes[0] = 0x03U;
    write_le32(bytes, 4U, static_cast<std::uint32_t>(rows.size()));
    write_le16(bytes, 8U, static_cast<std::uint16_t>(header_bytes));
    write_le16(bytes, 10U, static_cast<std::uint16_t>(record_bytes));

    for (std::size_t index = 0U; index < fields.size(); ++index) {
        const FieldSpec& field = fields[index];
        const std::size_t offset = 32U + index * 32U;
        for (std::size_t character = 0U; character < field.name.size() && character < 11U; ++character) {
            bytes[offset + character] = static_cast<unsigned char>(field.name[character]);
        }
        bytes[offset + 11U] = static_cast<unsigned char>(field.type);
        bytes[offset + 16U] = field.width;
        bytes[offset + 17U] = field.decimals;
    }
    bytes[header_bytes - 1U] = 0x0dU;

    for (std::size_t row_index = 0U; row_index < rows.size(); ++row_index) {
        const RowSpec& row = rows[row_index];
        std::size_t offset = header_bytes + row_index * record_bytes;
        bytes[offset++] = static_cast<unsigned char>(row.deleted ? '*' : ' ');
        for (std::size_t field_index = 0U; field_index < fields.size(); ++field_index) {
            const std::size_t width = fields[field_index].width;
            std::string value = row.values[field_index];
            if (value.size() < width) value.append(width - value.size(), ' ');
            value.resize(width);
            for (std::size_t character = 0U; character < width; ++character) {
                bytes[offset + character] = static_cast<unsigned char>(value[character]);
            }
            offset += width;
        }
    }
    if (eof_marker) bytes.back() = 0x1aU;
    return bytes;
}

class TempFile final {
public:
    explicit TempFile(const std::vector<unsigned char>& bytes) {
        const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() / ("aeris-dbf-" + std::to_string(stamp) + ".dbf");
        std::ofstream output(path_, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    ~TempFile() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

void test_round_trip_preserves_physical_rows() {
    const std::vector<FieldSpec> fields{{"NAME", 'C', 8U, 0U}, {"POP", 'N', 5U, 0U}};
    const std::vector<RowSpec> rows{
        {false, {"Alpha", "  42"}},
        {true, {"Beta", "   7"}},
    };
    TempFile file(build_dbf(fields, rows));
    const auto table = aeris::source::read_dbf_table(file.path());
    expect_true("valid table accepted", table.ok());
    expect_true("field count", table.fields.size() == 2U);
    expect_true("record count", table.records.size() == 2U);
    if (table.fields.size() == 2U) {
        expect_true("field name", table.fields[0].name == "NAME");
        expect_true("field type", table.fields[1].type == 'N');
        expect_true("field width", table.fields[1].width == 5U);
    }
    if (table.records.size() == 2U) {
        expect_true("physical row one", table.records[0].record_number == 1U);
        expect_true("physical row two", table.records[1].record_number == 2U);
        expect_true("active row preserved", !table.records[0].deleted);
        expect_true("deleted row preserved", table.records[1].deleted);
        expect_true("fixed width text preserved", table.records[0].values[0] == "Alpha   ");
        expect_true("fixed width numeric text preserved", table.records[0].values[1] == "  42 ");
    }
}

void test_no_eof_marker_is_valid() {
    TempFile file(build_dbf({{"NAME", 'C', 4U, 0U}}, {{false, {"AB"}}}, false));
    expect_true("optional EOF marker may be absent", aeris::source::read_dbf_table(file.path()).ok());
}

void test_unsupported_version_rejected() {
    auto bytes = build_dbf({{"NAME", 'C', 4U, 0U}}, {{false, {"AB"}}});
    bytes[0] = 0x83U;
    TempFile file(bytes);
    expect_true(
        "unsupported version rejected",
        aeris::source::read_dbf_table(file.path()).error == aeris::source::DbfError::unsupported_version
    );
}

void test_duplicate_field_rejected() {
    TempFile file(build_dbf({{"NAME", 'C', 4U, 0U}, {"NAME", 'C', 3U, 0U}}, {{false, {"AB", "CD"}}}));
    expect_true(
        "duplicate field rejected",
        aeris::source::read_dbf_table(file.path()).error == aeris::source::DbfError::duplicate_field
    );
}

void test_record_dimension_mismatch_rejected() {
    auto bytes = build_dbf({{"NAME", 'C', 4U, 0U}}, {{false, {"AB"}}}, false);
    // Claim one extra byte per record and append that byte so file-size accounting
    // remains internally consistent; the field-width invariant must still reject it.
    write_le16(bytes, 10U, 6U);
    bytes.push_back(static_cast<unsigned char>(' '));
    TempFile file(bytes);
    expect_true(
        "record dimensions rejected",
        aeris::source::read_dbf_table(file.path()).error == aeris::source::DbfError::record_length_mismatch
    );
}

void test_invalid_deletion_flag_rejected() {
    auto bytes = build_dbf({{"NAME", 'C', 4U, 0U}}, {{false, {"AB"}}});
    const std::size_t header_bytes = 32U + 32U + 1U;
    bytes[header_bytes] = static_cast<unsigned char>('!');
    TempFile file(bytes);
    const auto table = aeris::source::read_dbf_table(file.path());
    expect_true("bad deletion flag rejected", table.error == aeris::source::DbfError::malformed_record);
    expect_true("failed physical row reported", table.failed_record_number == 1U);
}

void test_header_terminator_rejected() {
    auto bytes = build_dbf({{"NAME", 'C', 4U, 0U}}, {{false, {"AB"}}});
    bytes[32U + 32U] = 0U;
    TempFile file(bytes);
    expect_true(
        "bad header terminator rejected",
        aeris::source::read_dbf_table(file.path()).error == aeris::source::DbfError::invalid_header
    );
}

void test_trailing_garbage_rejected() {
    auto bytes = build_dbf({{"NAME", 'C', 4U, 0U}}, {{false, {"AB"}}}, false);
    bytes.push_back(static_cast<unsigned char>('X'));
    TempFile file(bytes);
    expect_true(
        "non-EOF trailing byte rejected",
        aeris::source::read_dbf_table(file.path()).error == aeris::source::DbfError::file_length_mismatch
    );
}

}  // namespace

int main() {
    test_round_trip_preserves_physical_rows();
    test_no_eof_marker_is_valid();
    test_unsupported_version_rejected();
    test_duplicate_field_rejected();
    test_record_dimension_mismatch_rejected();
    test_invalid_deletion_flag_rejected();
    test_header_terminator_rejected();
    test_trailing_garbage_rejected();

    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "dbf_reader: PASS\n";
    return EXIT_SUCCESS;
}
