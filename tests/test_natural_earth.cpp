// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/source/acquisition.hpp"
#include "aeris/source/natural_earth.hpp"
#include "aeris/util/sha256.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
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
    std::memcpy(&bits, &value, sizeof(bits));
    for (std::size_t index = 0U; index < 8U; ++index) {
        bytes[offset + index] = static_cast<unsigned char>((bits >> (index * 8U)) & 0xffU);
    }
}

std::vector<unsigned char> make_land_shapefile() {
    constexpr std::size_t content_bytes = 44U + 4U + 5U * 16U;
    constexpr std::size_t file_bytes = 100U + 8U + content_bytes;
    std::vector<unsigned char> bytes(file_bytes, 0U);
    put_be_u32(bytes, 0U, 9994U);
    put_be_u32(bytes, 24U, static_cast<std::uint32_t>(file_bytes / 2U));
    put_le_u32(bytes, 28U, 1000U);
    put_le_u32(bytes, 32U, 5U);
    put_le_f64(bytes, 36U, 0.0); put_le_f64(bytes, 44U, 0.0);
    put_le_f64(bytes, 52U, 2.0); put_le_f64(bytes, 60U, 2.0);
    put_be_u32(bytes, 100U, 1U);
    put_be_u32(bytes, 104U, static_cast<std::uint32_t>(content_bytes / 2U));
    const std::size_t content = 108U;
    put_le_u32(bytes, content, 5U);
    put_le_f64(bytes, content + 4U, 0.0); put_le_f64(bytes, content + 12U, 0.0);
    put_le_f64(bytes, content + 20U, 2.0); put_le_f64(bytes, content + 28U, 2.0);
    put_le_u32(bytes, content + 36U, 1U);
    put_le_u32(bytes, content + 40U, 5U);
    put_le_u32(bytes, content + 44U, 0U);
    const std::array<std::array<double, 2>, 5> points{{
        {{0.0, 0.0}}, {{0.0, 2.0}}, {{2.0, 2.0}}, {{2.0, 0.0}}, {{0.0, 0.0}},
    }};
    const std::size_t points_offset = content + 48U;
    for (std::size_t index = 0U; index < points.size(); ++index) {
        put_le_f64(bytes, points_offset + index * 16U, points[index][0]);
        put_le_f64(bytes, points_offset + index * 16U + 8U, points[index][1]);
    }
    return bytes;
}

class FixtureDirectory final {
public:
    FixtureDirectory() {
        const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() / ("aeris-natural-earth-" + std::to_string(stamp));
        std::filesystem::create_directories(path_);
        const auto shp = make_land_shapefile();
        {
            std::ofstream output(path_ / "ne_110m_land.shp", std::ios::binary | std::ios::trunc);
            output.write(reinterpret_cast<const char*>(shp.data()), static_cast<std::streamsize>(shp.size()));
        }
        {
            std::ofstream output(path_ / "ne_110m_land.VERSION.txt", std::ios::binary | std::ios::trunc);
            output << "4.1.0\n";
        }
        {
            std::ofstream output(path_ / "ne_110m_land.prj", std::ios::binary | std::ios::trunc);
            output << "GEOGCS[\"GCS_WGS_1984\",DATUM[\"D_WGS_1984\"]]";
        }
    }

    ~FixtureDirectory() {
        std::error_code ignored{};
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] aeris::source::SnapshotManifest manifest() const {
        aeris::source::SnapshotManifest value{};
        value.provider = "Natural Earth";
        value.dataset = "ne_110m_land";
        value.snapshot = "v5.1.2-test";
        value.source_uri = "fixture://natural-earth/v5.1.2-test/ne_110m_land";
        value.retrieved_at_utc = "2026-08-12T00:00:00Z";
        add_resource(value, "geometry.shp", "ne_110m_land.shp");
        add_resource(value, "dataset.version", "ne_110m_land.VERSION.txt");
        add_resource(value, "crs.prj", "ne_110m_land.prj");
        return value;
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    void add_resource(
        aeris::source::SnapshotManifest& manifest,
        const std::string& logical,
        const std::filesystem::path& relative
    ) const {
        const auto full = path_ / relative;
        const auto hash = aeris::util::sha256_file(full);
        manifest.resources.push_back({logical, relative, hash.digest.hex(), std::filesystem::file_size(full)});
    }

    std::filesystem::path path_;
};

std::optional<aeris::source::VerifiedSnapshot> verified_fixture(const FixtureDirectory& fixture) {
    auto verified = aeris::source::verify_local_snapshot(fixture.path(), fixture.manifest());
    expect_true("Natural Earth fixture snapshot verifies", verified.ok());
    if (!verified.ok()) {
        return std::nullopt;
    }
    return std::move(verified.snapshot);
}

void test_adapter_loads_verified_snapshot() {
    const FixtureDirectory fixture{};
    auto snapshot = verified_fixture(fixture);
    if (!snapshot.has_value()) {
        return;
    }
    const std::string aggregate_hash = snapshot->content_sha256();
    const aeris::source::NaturalEarthLand110mAdapter adapter(std::move(*snapshot));
    const auto result = adapter.load({aeris::source::Capability::land, "v5.1.2-test", ""});
    expect_true("Natural Earth fixture loads", result.ok());
    if (!result.ok()) {
        return;
    }

    expect_true("provider recorded", result.provenance.provider == "Natural Earth");
    expect_true("dataset recorded", result.provenance.dataset == "ne_110m_land");
    expect_true("dataset version read", result.provenance.dataset_version == "4.1.0");
    expect_true("aggregate content hash recorded", result.provenance.content_sha256 == aggregate_hash);
    expect_true("one feature emitted", result.features.size() == 1U);
}

void test_wrong_snapshot_identity_fails_closed() {
    const FixtureDirectory fixture{};
    auto manifest = fixture.manifest();
    manifest.provider = "not-natural-earth";
    auto verified = aeris::source::verify_local_snapshot(fixture.path(), manifest);
    expect_true("altered provider snapshot still byte-verifies", verified.ok());
    if (!verified.ok()) {
        return;
    }
    const aeris::source::NaturalEarthLand110mAdapter adapter(std::move(*verified.snapshot));
    const auto result = adapter.load({aeris::source::Capability::land, "v5.1.2-test", ""});
    expect_true("adapter rejects wrong provider identity", result.error == aeris::source::SourceError::malformed_source);
}

void test_worldview_rejected_for_physical_land() {
    const FixtureDirectory fixture{};
    auto snapshot = verified_fixture(fixture);
    if (!snapshot.has_value()) {
        return;
    }
    const aeris::source::NaturalEarthLand110mAdapter adapter(std::move(*snapshot));
    const auto result = adapter.load({aeris::source::Capability::land, "v5.1.2-test", "political-view"});
    expect_true("physical land worldview rejected", result.error == aeris::source::SourceError::unsupported_worldview);
}

}  // namespace

int main() {
    test_adapter_loads_verified_snapshot();
    test_wrong_snapshot_identity_fails_closed();
    test_worldview_rejected_for_physical_land();

    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "natural_earth_adapter: PASS\n";
    return EXIT_SUCCESS;
}
