// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aeris::elevation {

inline constexpr std::string_view kElevationTileMediaType =
    "application/vnd.aeris.elevation-tile.v1";
inline constexpr std::int16_t kNoDataMeters = static_cast<std::int16_t>(-32768);
inline constexpr std::uint32_t kMaxTileDimension = 8192U;
inline constexpr std::uint64_t kMaxTileSamples = 64ULL * 1024ULL * 1024ULL;

enum class VerticalReference : std::uint8_t {
    unknown = 0U,
    egm2008_orthometric = 1U,
};

// Canonical geographic elevation tile. Bounds describe raster cell edges in
// integer micro-arcseconds so common global DEM spacings (60", 30", 15") are
// represented exactly and never drift through floating-point serialization.
// Samples are row-major, north-to-south then west-to-east, in whole metres.
struct ElevationTile final {
    std::uint32_t width{0U};
    std::uint32_t height{0U};
    std::int64_t west_microarcsec{0};
    std::int64_t north_microarcsec{0};
    std::int64_t longitude_step_microarcsec{0};
    std::int64_t latitude_step_microarcsec{0};
    VerticalReference vertical_reference{VerticalReference::unknown};
    std::vector<std::int16_t> samples_m;
};

struct ElevationTileDecodeResult final {
    std::optional<ElevationTile> tile;
    std::string diagnostic;

    [[nodiscard]] bool ok() const noexcept { return tile.has_value(); }
};

// Returns an empty string when the tile is canonical. Validation includes
// geographic bounds, sample count, supported vertical reference and reserved
// no-data semantics.
[[nodiscard]] std::string validate_elevation_tile(const ElevationTile& tile);

// Stable little-endian binary encoding for embedding in .aeris resources.
// The byte stream is independent of host endianness and compiler ABI.
[[nodiscard]] std::vector<std::uint8_t> encode_elevation_tile_v1(
    const ElevationTile& tile);

[[nodiscard]] ElevationTileDecodeResult decode_elevation_tile_v1(
    const std::uint8_t* data,
    std::size_t size);

[[nodiscard]] inline ElevationTileDecodeResult decode_elevation_tile_v1(
    const std::vector<std::uint8_t>& bytes) {
    return decode_elevation_tile_v1(bytes.data(), bytes.size());
}

// Bilinear sample at geographic lon/lat degrees. Cell values are interpreted at
// cell centers. Queries outside the tile, or interpolation touching no-data,
// return std::nullopt rather than silently fabricating elevation.
[[nodiscard]] std::optional<double> sample_elevation_m(
    const ElevationTile& tile,
    double longitude_deg,
    double latitude_deg);

}  // namespace aeris::elevation
