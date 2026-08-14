// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/source/shapefile.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void expect_true(const std::string_view name, const bool condition) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL " << name << '\n';
    }
}

void put_be_u32(std::vector<unsigned char>& bytes, const std::size_t offset, const std::uint32_t value) {
    bytes[offset] = static_cast<unsigned char>((value >> 24U) & 0xffU);
    bytes[offset + 1U] = static_cast<unsigned char>((value >> 16U) & 0xffU);
    bytes[offset + 2U] = static_cast<unsigned char>((value >> 8U) & 0xffU);
    bytes[offset + 3U] = static_cast<unsigned char>(value & 0xffU);
}

void put_le_u32(std::vector<unsigned char>& bytes, const std::size_t offset, const std::uint32_t value) {
    bytes[offset] = static_cast<unsigned char>(value & 0xffU);
    bytes[offset + 1U] = static_cast<unsigned char>((value >> 8U) & 0xffU);
    bytes[offset + 2U] = static_cast<unsigned char>((value >> 16U) & 0xffU);
    bytes[offset + 3U] = static_cast<unsigned char>((value >> 24U) & 0xffU);
}

void put_le_f64(std::vector<unsigned char>& bytes, const std::size_t offset, const double value) {
    std::uint64_t bits = 0U;
    static_assert(sizeof(bits) == sizeof(value), "unexpected double size");
    std::memcpy(&bits, &value, sizeof(bits));
    for (std::size_t index = 0U; index < 8U; ++index) {
        bytes[offset + index] = static_cast<unsigned char>((bits >> (index * 8U)) & 0xffU);
    }
}

struct XY final {
    double x = 0.0;
    double y = 0.0;
};

std::vector<unsigned char> make_polygon_fixture() {
    const std::array<XY, 5> outer{{
        {0.0, 0.0},
        {0.0, 2.0},
        {2.0, 2.0},
        {2.0, 0.0},
        {0.0, 0.0},
    }};
    const std::array<XY, 5> hole{{
        {0.5, 0.5},
        {1.5, 0.5},
        {1.5, 1.5},
        {0.5, 1.5},
        {0.5, 0.5},
    }};

    constexpr std::uint32_t part_count = 2U;
    constexpr std::uint32_t point_count = 10U;
    constexpr std::size_t content_bytes = 44U + part_count * 4U + point_count * 16U;
    constexpr std::size_t file_bytes = 100U + 8U + content_bytes;

    std::vector<unsigned char> bytes(file_bytes, 0U);
    put_be_u32(bytes, 0U, 9994U);
    put_be_u32(bytes, 24U, static_cast<std::uint32_t>(file_bytes / 2U));
    put_le_u32(bytes, 28U, 1000U);
    put_le_u32(bytes, 32U, 5U);
    put_le_f64(bytes, 36U, 0.0);
    put_le_f64(bytes, 44U, 0.0);
    put_le_f64(bytes, 52U, 2.0);
    put_le_f64(bytes, 60U, 2.0);

    const std::size_t record_header = 100U;
    put_be_u32(bytes, record_header, 1U);
    put_be_u32(bytes, record_header + 4U, static_cast<std::uint32_t>(content_bytes / 2U));

    const std::size_t content = record_header + 8U;
    put_le_u32(bytes, content, 5U);
    put_le_f64(bytes, content + 4U, 0.0);
    put_le_f64(bytes, content + 12U, 0.0);
    put_le_f64(bytes, content + 20U, 2.0);
    put_le_f64(bytes, content + 28U, 2.0);
    put_le_u32(bytes, content + 36U, part_count);
    put_le_u32(bytes, content + 40U, point_count);
    put_le_u32(bytes, content + 44U, 0U);
    put_le_u32(bytes, content + 48U, 5U);

    const std::size_t points = content + 52U;
    std::size_t point_index = 0U;
    for (const XY point : outer) {
        put_le_f64(bytes, points + point_index * 16U, point.x);
        put_le_f64(bytes, points + point_index * 16U + 8U, point.y);
        ++point_index;
    }
    for (const XY point : hole) {
        put_le_f64(bytes, points + point_index * 16U, point.x);
        put_le_f64(bytes, points + point_index * 16U + 8U, point.y);
        ++point_index;
    }

    return bytes;
}

std::vector<unsigned char> make_boundary_polygon_fixture(const double eastern_edge) {
    const std::array<XY, 5> outer{{
        {178.0, 0.0},
        {178.0, 2.0},
        {eastern_edge, 2.0},
        {eastern_edge, 0.0},
        {178.0, 0.0},
    }};

    constexpr std::uint32_t part_count = 1U;
    constexpr std::uint32_t point_count = 5U;
    constexpr std::size_t content_bytes = 44U + part_count * 4U + point_count * 16U;
    constexpr std::size_t file_bytes = 100U + 8U + content_bytes;

    std::vector<unsigned char> bytes(file_bytes, 0U);
    put_be_u32(bytes, 0U, 9994U);
    put_be_u32(bytes, 24U, static_cast<std::uint32_t>(file_bytes / 2U));
    put_le_u32(bytes, 28U, 1000U);
    put_le_u32(bytes, 32U, 5U);
    put_le_f64(bytes, 36U, 178.0);
    put_le_f64(bytes, 44U, 0.0);
    put_le_f64(bytes, 52U, 180.0);
    put_le_f64(bytes, 60U, 2.0);

    const std::size_t record_header = 100U;
    put_be_u32(bytes, record_header, 1U);
    put_be_u32(bytes, record_header + 4U, static_cast<std::uint32_t>(content_bytes / 2U));

    const std::size_t content = record_header + 8U;
    put_le_u32(bytes, content, 5U);
    put_le_f64(bytes, content + 4U, 178.0);
    put_le_f64(bytes, content + 12U, 0.0);
    put_le_f64(bytes, content + 20U, 180.0);
    put_le_f64(bytes, content + 28U, 2.0);
    put_le_u32(bytes, content + 36U, part_count);
    put_le_u32(bytes, content + 40U, point_count);
    put_le_u32(bytes, content + 44U, 0U);

    const std::size_t points = content + 48U;
    for (std::size_t index = 0U; index < outer.size(); ++index) {
        put_le_f64(bytes, points + index * 16U, outer[index].x);
        put_le_f64(bytes, points + index * 16U + 8U, outer[index].y);
    }

    return bytes;
}

class TempFile final {
public:
    explicit TempFile(const std::vector<unsigned char>& bytes) {
        const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
            ("aeris-shapefile-" + std::to_string(stamp) + ".shp");
        std::ofstream output(path_, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        output.close();
    }

    ~TempFile() {
        std::error_code ignored{};
        std::filesystem::remove(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void test_polygon_and_ring_roles() {
    const TempFile file(make_polygon_fixture());
    const auto result = aeris::source::read_polygon_shapefile(file.path());
    expect_true("synthetic Polygon Shapefile parses", result.ok());
    if (!result.ok()) {
        return;
    }

    expect_true("one record returned", result.records.size() == 1U);
    if (result.records.size() != 1U) {
        return;
    }

    const auto& record = result.records.front();
    expect_true("record number preserved", record.record_number == 1U);
    expect_true("two rings returned", record.rings.size() == 2U);
    expect_true("ordinary fixture needs no boundary normalization", result.normalized_boundary_coordinates == 0U);
    if (record.rings.size() == 2U) {
        expect_true("clockwise outer classified exterior", record.rings[0].role == aeris::source::RingRole::exterior);
        expect_true("counterclockwise hole classified interior", record.rings[1].role == aeris::source::RingRole::interior);
        expect_true("duplicate terminal point removed", record.rings[0].geometry.vertices.size() == 4U);
    }
}

void test_boundary_roundoff_is_canonicalized_by_ulp() {
    double longitude = 180.0;
    for (unsigned step = 0U; step < 5U; ++step) {
        longitude = std::nextafter(longitude, std::numeric_limits<double>::infinity());
    }

    const TempFile file(make_boundary_polygon_fixture(longitude));
    const auto result = aeris::source::read_polygon_shapefile(file.path());
    expect_true("five-ULP longitude tail is accepted", result.ok());
    expect_true("two repeated eastern vertices are normalized", result.normalized_boundary_coordinates == 2U);
}

void test_material_boundary_violation_is_rejected() {
    double longitude = 180.0;
    for (unsigned step = 0U; step < 16U; ++step) {
        longitude = std::nextafter(longitude, std::numeric_limits<double>::infinity());
    }

    const TempFile file(make_boundary_polygon_fixture(longitude));
    const auto result = aeris::source::read_polygon_shapefile(file.path());
    expect_true(
        "sixteen-ULP longitude violation is rejected",
        result.error == aeris::source::ShapefileError::invalid_coordinate
    );
}

void test_bad_file_code_rejected() {
    auto bytes = make_polygon_fixture();
    put_be_u32(bytes, 0U, 1234U);
    const TempFile file(bytes);
    const auto result = aeris::source::read_polygon_shapefile(file.path());
    expect_true("bad file code rejected", result.error == aeris::source::ShapefileError::invalid_file_code);
}

void test_declared_length_mismatch_rejected() {
    auto bytes = make_polygon_fixture();
    put_be_u32(bytes, 24U, 50U);
    const TempFile file(bytes);
    const auto result = aeris::source::read_polygon_shapefile(file.path());
    expect_true(
        "declared length mismatch rejected",
        result.error == aeris::source::ShapefileError::file_length_mismatch
    );
}

void test_truncation_rejected() {
    auto bytes = make_polygon_fixture();
    bytes.resize(bytes.size() - 7U);
    put_be_u32(bytes, 24U, static_cast<std::uint32_t>(bytes.size() / 2U));
    const TempFile file(bytes);
    const auto result = aeris::source::read_polygon_shapefile(file.path());
    expect_true("truncated record rejected", !result.ok());
}

}  // namespace

int main() {
    test_polygon_and_ring_roles();
    test_boundary_roundoff_is_canonicalized_by_ulp();
    test_material_boundary_violation_is_rejected();
    test_bad_file_code_rejected();
    test_declared_length_mismatch_rejected();
    test_truncation_rejected();

    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return EXIT_FAILURE;
    }

    std::cout << "shapefile_reader: PASS\n";
    return EXIT_SUCCESS;
}
