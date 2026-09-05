// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/elevation/grid.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

namespace aeris::elevation {
namespace {

constexpr std::array<std::uint8_t, 8U> kMagic{
    static_cast<std::uint8_t>('A'),
    static_cast<std::uint8_t>('E'),
    static_cast<std::uint8_t>('R'),
    static_cast<std::uint8_t>('E'),
    static_cast<std::uint8_t>('L'),
    static_cast<std::uint8_t>('V'),
    static_cast<std::uint8_t>('1'),
    0U,
};
constexpr std::size_t kHeaderBytes = 64U;
constexpr std::int64_t kMicroArcsecondsPerDegree = 3600LL * 1000000LL;
constexpr std::int64_t kLongitudeLimit = 180LL * kMicroArcsecondsPerDegree;
constexpr std::int64_t kLatitudeLimit = 90LL * kMicroArcsecondsPerDegree;

void append_u16(std::vector<std::uint8_t>& out, const std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xffU));
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
}

void append_u32(std::vector<std::uint8_t>& out, const std::uint32_t value) {
    for (unsigned shift = 0U; shift < 32U; shift += 8U) {
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

void append_u64(std::vector<std::uint8_t>& out, const std::uint64_t value) {
    for (unsigned shift = 0U; shift < 64U; shift += 8U) {
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

void append_i64(std::vector<std::uint8_t>& out, const std::int64_t value) {
    append_u64(out, static_cast<std::uint64_t>(value));
}

void append_i16(std::vector<std::uint8_t>& out, const std::int16_t value) {
    append_u16(out, static_cast<std::uint16_t>(value));
}

[[nodiscard]] std::uint16_t read_u16(const std::uint8_t* data) noexcept {
    return static_cast<std::uint16_t>(data[0]) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[1]) << 8U);
}

[[nodiscard]] std::uint32_t read_u32(const std::uint8_t* data) noexcept {
    std::uint32_t value = 0U;
    for (unsigned index = 0U; index < 4U; ++index) {
        value |= static_cast<std::uint32_t>(data[index]) << (index * 8U);
    }
    return value;
}

[[nodiscard]] std::uint64_t read_u64(const std::uint8_t* data) noexcept {
    std::uint64_t value = 0U;
    for (unsigned index = 0U; index < 8U; ++index) {
        value |= static_cast<std::uint64_t>(data[index]) << (index * 8U);
    }
    return value;
}

[[nodiscard]] std::int64_t decode_i64(const std::uint64_t bits) noexcept {
    if (bits <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return static_cast<std::int64_t>(bits);
    }
    const std::uint64_t magnitude = (~bits) + 1ULL;
    if (magnitude == (1ULL << 63U)) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return -static_cast<std::int64_t>(magnitude);
}

[[nodiscard]] std::int16_t decode_i16(const std::uint16_t bits) noexcept {
    if (bits <= static_cast<std::uint16_t>(std::numeric_limits<std::int16_t>::max())) {
        return static_cast<std::int16_t>(bits);
    }
    const std::uint32_t magnitude = 0x10000U - static_cast<std::uint32_t>(bits);
    return static_cast<std::int16_t>(-static_cast<std::int32_t>(magnitude));
}

[[nodiscard]] bool valid_vertical_reference(const VerticalReference value) noexcept {
    return value == VerticalReference::unknown ||
        value == VerticalReference::egm2008_orthometric;
}

[[nodiscard]] bool checked_sample_count(
    const std::uint32_t width,
    const std::uint32_t height,
    std::uint64_t& count
) noexcept {
    count = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
    return count <= kMaxTileSamples;
}

}  // namespace

std::string validate_elevation_tile(const ElevationTile& tile) {
    if (tile.width < 2U || tile.height < 2U ||
        tile.width > kMaxTileDimension || tile.height > kMaxTileDimension) {
        return "elevation tile dimensions must be within 2..8192";
    }

    std::uint64_t expected_samples = 0U;
    if (!checked_sample_count(tile.width, tile.height, expected_samples)) {
        return "elevation tile exceeds the canonical sample-count bound";
    }
    if (expected_samples != static_cast<std::uint64_t>(tile.samples_m.size())) {
        return "elevation tile sample count does not match width*height";
    }
    if (tile.longitude_step_microarcsec <= 0 ||
        tile.latitude_step_microarcsec <= 0) {
        return "elevation tile geographic steps must be positive";
    }
    if (!valid_vertical_reference(tile.vertical_reference)) {
        return "elevation tile vertical reference is unsupported";
    }
    if (tile.west_microarcsec < -kLongitudeLimit ||
        tile.west_microarcsec >= kLongitudeLimit ||
        tile.north_microarcsec <= -kLatitudeLimit ||
        tile.north_microarcsec > kLatitudeLimit) {
        return "elevation tile north/west edge is outside geographic bounds";
    }

    const std::int64_t width = static_cast<std::int64_t>(tile.width);
    const std::int64_t height = static_cast<std::int64_t>(tile.height);
    if (tile.longitude_step_microarcsec >
            std::numeric_limits<std::int64_t>::max() / width ||
        tile.latitude_step_microarcsec >
            std::numeric_limits<std::int64_t>::max() / height) {
        return "elevation tile geographic extent overflows canonical bounds";
    }
    const std::int64_t east = tile.west_microarcsec +
        width * tile.longitude_step_microarcsec;
    const std::int64_t south = tile.north_microarcsec -
        height * tile.latitude_step_microarcsec;
    if (east <= tile.west_microarcsec || east > kLongitudeLimit ||
        south >= tile.north_microarcsec || south < -kLatitudeLimit) {
        return "elevation tile geographic extent is outside lon/lat bounds";
    }
    return {};
}

std::vector<std::uint8_t> encode_elevation_tile_v1(const ElevationTile& tile) {
    if (!validate_elevation_tile(tile).empty()) return {};

    const std::uint64_t sample_count = static_cast<std::uint64_t>(tile.samples_m.size());
    if (sample_count >
        (static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) -
         static_cast<std::uint64_t>(kHeaderBytes)) / 2ULL) {
        return {};
    }

    std::vector<std::uint8_t> bytes;
    bytes.reserve(kHeaderBytes + tile.samples_m.size() * 2U);
    bytes.insert(bytes.end(), kMagic.begin(), kMagic.end());
    append_u32(bytes, tile.width);
    append_u32(bytes, tile.height);
    append_i64(bytes, tile.west_microarcsec);
    append_i64(bytes, tile.north_microarcsec);
    append_i64(bytes, tile.longitude_step_microarcsec);
    append_i64(bytes, tile.latitude_step_microarcsec);
    bytes.push_back(static_cast<std::uint8_t>(tile.vertical_reference));
    for (std::size_t index = 0U; index < 7U; ++index) bytes.push_back(0U);
    append_u64(bytes, sample_count);
    for (const std::int16_t sample : tile.samples_m) append_i16(bytes, sample);
    return bytes;
}

ElevationTileDecodeResult decode_elevation_tile_v1(
    const std::uint8_t* data,
    const std::size_t size
) {
    if (data == nullptr || size < kHeaderBytes) {
        return {std::nullopt, "elevation tile payload is shorter than the v1 header"};
    }
    if (!std::equal(kMagic.begin(), kMagic.end(), data)) {
        return {std::nullopt, "elevation tile magic/version is not AERELV1"};
    }

    ElevationTile tile{};
    tile.width = read_u32(data + 8U);
    tile.height = read_u32(data + 12U);
    tile.west_microarcsec = decode_i64(read_u64(data + 16U));
    tile.north_microarcsec = decode_i64(read_u64(data + 24U));
    tile.longitude_step_microarcsec = decode_i64(read_u64(data + 32U));
    tile.latitude_step_microarcsec = decode_i64(read_u64(data + 40U));
    tile.vertical_reference = static_cast<VerticalReference>(data[48U]);
    for (std::size_t index = 49U; index < 56U; ++index) {
        if (data[index] != 0U) {
            return {std::nullopt, "elevation tile reserved v1 header bytes are nonzero"};
        }
    }

    const std::uint64_t declared_samples = read_u64(data + 56U);
    std::uint64_t expected_samples = 0U;
    if (!checked_sample_count(tile.width, tile.height, expected_samples) ||
        declared_samples != expected_samples) {
        return {std::nullopt, "elevation tile declared sample count is noncanonical"};
    }
    if (declared_samples >
        (static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) -
         static_cast<std::uint64_t>(kHeaderBytes)) / 2ULL) {
        return {std::nullopt, "elevation tile byte count exceeds this process address space"};
    }
    const std::size_t expected_size = kHeaderBytes +
        static_cast<std::size_t>(declared_samples) * 2U;
    if (size != expected_size) {
        return {std::nullopt, "elevation tile payload length does not match its header"};
    }

    tile.samples_m.reserve(static_cast<std::size_t>(declared_samples));
    const std::uint8_t* payload = data + kHeaderBytes;
    for (std::uint64_t index = 0U; index < declared_samples; ++index) {
        tile.samples_m.push_back(decode_i16(read_u16(
            payload + static_cast<std::size_t>(index) * 2U
        )));
    }

    std::string diagnostic = validate_elevation_tile(tile);
    if (!diagnostic.empty()) return {std::nullopt, std::move(diagnostic)};
    return {std::move(tile), {}};
}

std::optional<double> sample_elevation_m(
    const ElevationTile& tile,
    const double longitude_deg,
    const double latitude_deg
) noexcept {
    if (!std::isfinite(longitude_deg) || !std::isfinite(latitude_deg) ||
        !validate_elevation_tile(tile).empty()) {
        return std::nullopt;
    }

    const double longitude_microarcsec =
        longitude_deg * static_cast<double>(kMicroArcsecondsPerDegree);
    const double latitude_microarcsec =
        latitude_deg * static_cast<double>(kMicroArcsecondsPerDegree);
    const double x =
        (longitude_microarcsec - static_cast<double>(tile.west_microarcsec)) /
            static_cast<double>(tile.longitude_step_microarcsec) -
        0.5;
    const double y =
        (static_cast<double>(tile.north_microarcsec) - latitude_microarcsec) /
            static_cast<double>(tile.latitude_step_microarcsec) -
        0.5;

    const double max_x = static_cast<double>(tile.width - 1U);
    const double max_y = static_cast<double>(tile.height - 1U);
    if (x < 0.0 || y < 0.0 || x > max_x || y > max_y) return std::nullopt;

    const auto x0 = static_cast<std::uint32_t>(std::floor(x));
    const auto y0 = static_cast<std::uint32_t>(std::floor(y));
    const std::uint32_t x1 = std::min(x0 + 1U, tile.width - 1U);
    const std::uint32_t y1 = std::min(y0 + 1U, tile.height - 1U);
    const double tx = x - static_cast<double>(x0);
    const double ty = y - static_cast<double>(y0);

    const auto sample = [&](const std::uint32_t sx, const std::uint32_t sy)
        -> std::optional<double> {
        const std::size_t index = static_cast<std::size_t>(sy) *
            static_cast<std::size_t>(tile.width) + static_cast<std::size_t>(sx);
        const std::int16_t value = tile.samples_m[index];
        if (value == kNoDataMeters) return std::nullopt;
        return static_cast<double>(value);
    };

    const auto z00 = sample(x0, y0);
    const auto z10 = sample(x1, y0);
    const auto z01 = sample(x0, y1);
    const auto z11 = sample(x1, y1);
    if (!z00 || !z10 || !z01 || !z11) return std::nullopt;

    const double north = *z00 + (*z10 - *z00) * tx;
    const double south = *z01 + (*z11 - *z01) * tx;
    return north + (south - north) * ty;
}

}  // namespace aeris::elevation
