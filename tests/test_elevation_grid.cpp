// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/elevation/grid.hpp"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr std::int64_t kDegree = 3600LL * 1000000LL;

int fail(const int code, const std::string& message) {
    std::cerr << "aeris_test_elevation_grid: FAIL " << message << '\n';
    return code;
}

bool near(const double left, const double right) {
    return std::abs(left - right) <= 1e-12;
}

}  // namespace

int main() {
    aeris::elevation::ElevationTile tile{};
    tile.width = 2U;
    tile.height = 2U;
    tile.west_microarcsec = -2LL * kDegree;
    tile.north_microarcsec = 2LL * kDegree;
    tile.longitude_step_microarcsec = kDegree;
    tile.latitude_step_microarcsec = kDegree;
    tile.vertical_reference = aeris::elevation::VerticalReference::egm2008_orthometric;
    tile.samples_m = {-100, 100, 300, 500};

    if (!aeris::elevation::validate_elevation_tile(tile).empty()) {
        return fail(1, "valid tile was rejected");
    }

    const std::vector<std::uint8_t> encoded =
        aeris::elevation::encode_elevation_tile_v1(tile);
    if (encoded.size() != 72U) {
        return fail(2, "2x2 tile did not use the canonical 64-byte header + payload");
    }

    const auto decoded = aeris::elevation::decode_elevation_tile_v1(encoded);
    if (!decoded.ok() || !decoded.tile.has_value()) {
        return fail(3, "encoded tile did not round-trip: " + decoded.diagnostic);
    }
    if (decoded.tile->width != tile.width || decoded.tile->height != tile.height ||
        decoded.tile->west_microarcsec != tile.west_microarcsec ||
        decoded.tile->north_microarcsec != tile.north_microarcsec ||
        decoded.tile->longitude_step_microarcsec != tile.longitude_step_microarcsec ||
        decoded.tile->latitude_step_microarcsec != tile.latitude_step_microarcsec ||
        decoded.tile->vertical_reference != tile.vertical_reference ||
        decoded.tile->samples_m != tile.samples_m) {
        return fail(4, "round-trip changed canonical tile fields");
    }

    const auto north_west = aeris::elevation::sample_elevation_m(tile, -1.5, 1.5);
    const auto center = aeris::elevation::sample_elevation_m(tile, -1.0, 1.0);
    const auto south_east = aeris::elevation::sample_elevation_m(tile, -0.5, 0.5);
    if (!north_west || !near(*north_west, -100.0) ||
        !center || !near(*center, 200.0) ||
        !south_east || !near(*south_east, 500.0)) {
        return fail(5, "bilinear geographic sampling is incorrect");
    }
    if (aeris::elevation::sample_elevation_m(tile, -2.1, 1.0).has_value() ||
        aeris::elevation::sample_elevation_m(tile, -1.0, -0.1).has_value()) {
        return fail(6, "sampling outside tile cell centers did not reject the query");
    }

    aeris::elevation::ElevationTile nodata = tile;
    nodata.samples_m[3] = aeris::elevation::kNoDataMeters;
    if (aeris::elevation::sample_elevation_m(nodata, -1.0, 1.0).has_value()) {
        return fail(7, "bilinear interpolation crossed canonical no-data");
    }

    std::vector<std::uint8_t> corrupt = encoded;
    corrupt[49U] = 1U;
    if (aeris::elevation::decode_elevation_tile_v1(corrupt).ok()) {
        return fail(8, "nonzero reserved header byte was accepted");
    }
    corrupt = encoded;
    corrupt.pop_back();
    if (aeris::elevation::decode_elevation_tile_v1(corrupt).ok()) {
        return fail(9, "truncated elevation payload was accepted");
    }

    aeris::elevation::ElevationTile invalid = tile;
    invalid.width = 0U;
    if (aeris::elevation::validate_elevation_tile(invalid).empty() ||
        !aeris::elevation::encode_elevation_tile_v1(invalid).empty()) {
        return fail(10, "invalid dimensions passed canonical validation/encoding");
    }

    invalid = tile;
    invalid.west_microarcsec = 179LL * kDegree;
    invalid.longitude_step_microarcsec = 2LL * kDegree;
    if (aeris::elevation::validate_elevation_tile(invalid).empty()) {
        return fail(11, "tile extending beyond +180 longitude was accepted");
    }

    std::cout
        << "aeris_test_elevation_grid: PASS"
        << " bytes=" << encoded.size()
        << " center_m=" << *center
        << '\n';
    return EXIT_SUCCESS;
}
