// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/source/shapefile.hpp"

#include "aeris/geo/wgs84.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <system_error>
#include <vector>

namespace aeris::source {
namespace {

constexpr std::uint32_t kFileCode = 9994U;
constexpr std::uint32_t kVersion = 1000U;
constexpr std::uint32_t kNullShape = 0U;
constexpr std::uint32_t kPolygonShape = 5U;
constexpr std::size_t kMainHeaderBytes = 100U;
constexpr std::size_t kRecordHeaderBytes = 8U;
constexpr double kDegreesToRadians = geo::kPi / 180.0;

static_assert(sizeof(double) == 8U, "AERIS Shapefile reader requires 64-bit double");
static_assert(std::numeric_limits<double>::is_iec559, "AERIS Shapefile reader requires IEEE-754 double");

[[nodiscard]] std::uint32_t be_u32(const unsigned char* data) noexcept {
    return (static_cast<std::uint32_t>(data[0]) << 24U) |
           (static_cast<std::uint32_t>(data[1]) << 16U) |
           (static_cast<std::uint32_t>(data[2]) << 8U) |
           static_cast<std::uint32_t>(data[3]);
}

[[nodiscard]] std::uint32_t le_u32(const unsigned char* data) noexcept {
    return static_cast<std::uint32_t>(data[0]) |
           (static_cast<std::uint32_t>(data[1]) << 8U) |
           (static_cast<std::uint32_t>(data[2]) << 16U) |
           (static_cast<std::uint32_t>(data[3]) << 24U);
}

[[nodiscard]] double le_f64(const unsigned char* data) noexcept {
    std::uint64_t bits =
        static_cast<std::uint64_t>(data[0]) |
        (static_cast<std::uint64_t>(data[1]) << 8U) |
        (static_cast<std::uint64_t>(data[2]) << 16U) |
        (static_cast<std::uint64_t>(data[3]) << 24U) |
        (static_cast<std::uint64_t>(data[4]) << 32U) |
        (static_cast<std::uint64_t>(data[5]) << 40U) |
        (static_cast<std::uint64_t>(data[6]) << 48U) |
        (static_cast<std::uint64_t>(data[7]) << 56U);
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

[[nodiscard]] bool read_exact(
    std::ifstream& input,
    unsigned char* destination,
    const std::size_t count
) {
    input.read(reinterpret_cast<char*>(destination), static_cast<std::streamsize>(count));
    return input.good() || input.gcount() == static_cast<std::streamsize>(count);
}

[[nodiscard]] double signed_xy_area(
    const std::vector<geometry::GeodeticPoint>& points
) noexcept {
    if (points.size() < 3U) {
        return 0.0;
    }

    long double sum = 0.0L;
    for (std::size_t index = 0U; index < points.size(); ++index) {
        const auto& a = points[index];
        const auto& b = points[(index + 1U) % points.size()];
        sum += static_cast<long double>(a.longitude_rad) * static_cast<long double>(b.latitude_rad) -
               static_cast<long double>(b.longitude_rad) * static_cast<long double>(a.latitude_rad);
    }
    return static_cast<double>(0.5L * sum);
}

[[nodiscard]] bool finite_box(const unsigned char* data) noexcept {
    for (std::size_t index = 0U; index < 4U; ++index) {
        if (!std::isfinite(le_f64(data + index * 8U))) {
            return false;
        }
    }
    return true;
}

}  // namespace

ShapefilePolygonResult read_polygon_shapefile(const std::filesystem::path& path) {
    ShapefilePolygonResult result{};

    std::error_code size_error{};
    const std::uintmax_t file_size = std::filesystem::file_size(path, size_error);
    if (size_error) {
        result.error = ShapefileError::io_error;
        result.diagnostic = "unable to determine .shp file size";
        return result;
    }
    if (file_size < kMainHeaderBytes) {
        result.error = ShapefileError::truncated_file;
        result.diagnostic = "file is smaller than the 100-byte Shapefile header";
        return result;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        result.error = ShapefileError::io_error;
        result.diagnostic = "unable to open .shp file";
        return result;
    }

    std::array<unsigned char, kMainHeaderBytes> header{};
    if (!read_exact(input, header.data(), header.size())) {
        result.error = ShapefileError::truncated_file;
        result.diagnostic = "unable to read complete Shapefile header";
        return result;
    }

    if (be_u32(header.data()) != kFileCode) {
        result.error = ShapefileError::invalid_file_code;
        result.diagnostic = "unexpected Shapefile file code";
        return result;
    }
    if (le_u32(header.data() + 28U) != kVersion) {
        result.error = ShapefileError::invalid_version;
        result.diagnostic = "unsupported Shapefile version";
        return result;
    }
    if (le_u32(header.data() + 32U) != kPolygonShape) {
        result.error = ShapefileError::unsupported_shape_type;
        result.diagnostic = "reader accepts Polygon shape type 5 only";
        return result;
    }
    if (!finite_box(header.data() + 36U)) {
        result.error = ShapefileError::invalid_coordinate;
        result.diagnostic = "non-finite file bounding box";
        return result;
    }

    const std::uint64_t declared_bytes = static_cast<std::uint64_t>(be_u32(header.data() + 24U)) * 2ULL;
    if (declared_bytes != file_size) {
        result.error = ShapefileError::file_length_mismatch;
        result.diagnostic = "Shapefile header length does not match actual file length";
        return result;
    }

    std::uint64_t consumed = kMainHeaderBytes;
    std::uint32_t expected_record_number = 1U;

    while (consumed < declared_bytes) {
        if (declared_bytes - consumed < kRecordHeaderBytes) {
            result.error = ShapefileError::truncated_file;
            result.diagnostic = "truncated record header";
            return result;
        }

        std::array<unsigned char, kRecordHeaderBytes> record_header{};
        if (!read_exact(input, record_header.data(), record_header.size())) {
            result.error = ShapefileError::truncated_file;
            result.diagnostic = "unable to read record header";
            return result;
        }
        consumed += kRecordHeaderBytes;

        const std::uint32_t record_number = be_u32(record_header.data());
        if (record_number != expected_record_number) {
            result.error = ShapefileError::malformed_record;
            result.failed_record_number = record_number;
            result.diagnostic = "record numbers are not consecutive from one";
            return result;
        }
        ++expected_record_number;

        const std::uint64_t content_bytes =
            static_cast<std::uint64_t>(be_u32(record_header.data() + 4U)) * 2ULL;
        if (content_bytes < 4ULL || content_bytes > declared_bytes - consumed ||
            content_bytes > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
            result.error = ShapefileError::truncated_file;
            result.failed_record_number = record_number;
            result.diagnostic = "record content length is outside file bounds";
            return result;
        }

        std::vector<unsigned char> content(static_cast<std::size_t>(content_bytes));
        if (!read_exact(input, content.data(), content.size())) {
            result.error = ShapefileError::truncated_file;
            result.failed_record_number = record_number;
            result.diagnostic = "unable to read complete record content";
            return result;
        }
        consumed += content_bytes;

        const std::uint32_t shape_type = le_u32(content.data());
        if (shape_type == kNullShape) {
            if (content.size() != 4U) {
                result.error = ShapefileError::malformed_record;
                result.failed_record_number = record_number;
                result.diagnostic = "Null Shape record contains unexpected trailing data";
                return result;
            }
            continue;
        }
        if (shape_type != kPolygonShape) {
            result.error = ShapefileError::unsupported_shape_type;
            result.failed_record_number = record_number;
            result.diagnostic = "record shape type does not match Polygon file type";
            return result;
        }

        constexpr std::size_t fixed_polygon_bytes = 44U;
        if (content.size() < fixed_polygon_bytes || !finite_box(content.data() + 4U)) {
            result.error = content.size() < fixed_polygon_bytes
                ? ShapefileError::malformed_record
                : ShapefileError::invalid_coordinate;
            result.failed_record_number = record_number;
            result.diagnostic = "invalid Polygon fixed record section";
            return result;
        }

        const std::uint32_t part_count = le_u32(content.data() + 36U);
        const std::uint32_t point_count = le_u32(content.data() + 40U);
        if (part_count == 0U || point_count == 0U || part_count > point_count) {
            result.error = ShapefileError::malformed_record;
            result.failed_record_number = record_number;
            result.diagnostic = "invalid Polygon part/point counts";
            return result;
        }

        const std::uint64_t expected_bytes =
            static_cast<std::uint64_t>(fixed_polygon_bytes) +
            static_cast<std::uint64_t>(part_count) * 4ULL +
            static_cast<std::uint64_t>(point_count) * 16ULL;
        if (expected_bytes != content.size()) {
            result.error = ShapefileError::malformed_record;
            result.failed_record_number = record_number;
            result.diagnostic = "Polygon record byte count does not match part/point counts";
            return result;
        }

        const std::size_t parts_offset = fixed_polygon_bytes;
        const std::size_t points_offset = parts_offset + static_cast<std::size_t>(part_count) * 4U;
        std::vector<std::uint32_t> part_starts;
        part_starts.reserve(part_count);
        for (std::uint32_t part = 0U; part < part_count; ++part) {
            const std::uint32_t start = le_u32(content.data() + parts_offset + static_cast<std::size_t>(part) * 4U);
            if ((part == 0U && start != 0U) || start >= point_count ||
                (!part_starts.empty() && start <= part_starts.back())) {
                result.error = ShapefileError::malformed_record;
                result.failed_record_number = record_number;
                result.diagnostic = "invalid Polygon part index table";
                return result;
            }
            part_starts.push_back(start);
        }

        ShapefileRecord record{};
        record.record_number = record_number;
        record.rings.reserve(part_count);

        for (std::uint32_t part = 0U; part < part_count; ++part) {
            const std::uint32_t start = part_starts[part];
            const std::uint32_t end = part + 1U < part_count ? part_starts[part + 1U] : point_count;
            if (end - start < 4U) {
                result.error = ShapefileError::degenerate_ring;
                result.failed_record_number = record_number;
                result.diagnostic = "Polygon ring contains fewer than four Shapefile points";
                return result;
            }

            std::vector<geometry::GeodeticPoint> source_points;
            source_points.reserve(static_cast<std::size_t>(end - start));
            for (std::uint32_t point = start; point < end; ++point) {
                const std::size_t offset = points_offset + static_cast<std::size_t>(point) * 16U;
                const double longitude_deg = le_f64(content.data() + offset);
                const double latitude_deg = le_f64(content.data() + offset + 8U);
                if (!std::isfinite(longitude_deg) || !std::isfinite(latitude_deg) ||
                    longitude_deg < -180.0 || longitude_deg > 180.0 ||
                    latitude_deg < -90.0 || latitude_deg > 90.0) {
                    result.error = ShapefileError::invalid_coordinate;
                    result.failed_record_number = record_number;
                    result.diagnostic = "Polygon point is outside valid WGS84 longitude/latitude bounds";
                    return result;
                }
                source_points.push_back({
                    longitude_deg * kDegreesToRadians,
                    latitude_deg * kDegreesToRadians,
                });
            }

            const double orientation = signed_xy_area(source_points);
            if (!std::isfinite(orientation) || orientation == 0.0) {
                result.error = ShapefileError::degenerate_ring;
                result.failed_record_number = record_number;
                result.diagnostic = "Polygon ring has zero or non-finite signed area in source coordinates";
                return result;
            }

            const geometry::LinearRingResult canonical =
                geometry::canonicalize_wgs84_linear_ring(source_points);
            if (!canonical.ok()) {
                result.error = ShapefileError::canonicalization_failed;
                result.geographic_error = canonical.error;
                result.failed_record_number = record_number;
                result.diagnostic = "Polygon ring cannot be normalized to canonical AERIS geometry";
                return result;
            }

            ShapefileRing ring{};
            ring.geometry = canonical.value;
            // ESRI Shapefile Polygon convention: exterior rings are clockwise
            // and holes are counterclockwise in source x/y coordinates.
            ring.role = orientation < 0.0 ? RingRole::exterior : RingRole::interior;
            record.rings.push_back(std::move(ring));
        }

        result.records.push_back(std::move(record));
    }

    if (consumed != declared_bytes) {
        result.error = ShapefileError::file_length_mismatch;
        result.diagnostic = "reader did not terminate exactly at declared file length";
    }

    return result;
}

}  // namespace aeris::source
