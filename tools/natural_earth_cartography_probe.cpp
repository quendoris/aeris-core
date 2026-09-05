// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/source/acquisition.hpp"
#include "aeris/source/natural_earth_cartography.hpp"
#include "aeris/util/sha256.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace {

constexpr std::string_view kProvider = "Natural Earth";
constexpr std::string_view kDataset = "ne_110m_admin_0_countries";
constexpr std::string_view kSnapshot = "v5.1.2";
constexpr std::string_view kSourceUri =
    "https://github.com/nvkelso/natural-earth-vector/tree/f1890d9f152c896d250a77557a5751a93d494776/110m_cultural";

int fail(const int code, const std::string& diagnostic) {
    std::cerr << "Natural Earth cartography proof failed: " << diagnostic << '\n';
    return code;
}

bool add_resource(
    aeris::source::SnapshotManifest& manifest,
    const std::filesystem::path& root,
    std::string logical_name,
    std::filesystem::path relative_path
) {
    const std::filesystem::path full = root / relative_path;
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(full, error);
    if (error) return false;
    const aeris::util::Sha256FileResult hash = aeris::util::sha256_file(full);
    if (!hash.ok()) return false;
    manifest.resources.push_back({
        std::move(logical_name),
        std::move(relative_path),
        hash.digest.hex(),
        size,
    });
    return true;
}

const aeris::source::FeatureProperty* property(
    const aeris::source::Feature& feature,
    const std::string_view key
) {
    for (const auto& candidate : feature.properties) {
        if (candidate.key == key) return &candidate;
    }
    return nullptr;
}

}  // namespace

int main(const int argc, char** argv) {
    if (argc != 3) return fail(2, "usage: <snapshot-root> <retrieved-at-utc>");

    const std::filesystem::path root = argv[1];
    aeris::source::SnapshotManifest manifest{};
    manifest.provider = std::string(kProvider);
    manifest.dataset = std::string(kDataset);
    manifest.snapshot = std::string(kSnapshot);
    manifest.source_uri = std::string(kSourceUri);
    manifest.retrieved_at_utc = argv[2];

    if (!add_resource(manifest, root, "geometry.shp", "ne_110m_admin_0_countries.shp") ||
        !add_resource(manifest, root, "attributes.dbf", "ne_110m_admin_0_countries.dbf") ||
        !add_resource(manifest, root, "attributes.cpg", "ne_110m_admin_0_countries.cpg") ||
        !add_resource(manifest, root, "crs.prj", "ne_110m_admin_0_countries.prj") ||
        !add_resource(manifest, root, "dataset.version", "ne_110m_admin_0_countries.VERSION.txt")) {
        return fail(3, "could not hash all pinned admin0 resources");
    }

    auto verified = aeris::source::verify_local_snapshot(root, manifest);
    if (!verified.ok() || !verified.snapshot.has_value()) {
        return fail(4, "snapshot verification failed: " + verified.diagnostic);
    }

    aeris::source::Request request{};
    request.capability = aeris::source::Capability::admin0;
    request.snapshot = std::string(kSnapshot);
    request.worldview = "natural-earth.de-facto";

    aeris::source::NaturalEarthAdmin0Cartography110mAdapter adapter{};
    const aeris::source::Result result = adapter.load(*verified.snapshot, request);
    if (!result.ok() || !result.feature_properties_complete || result.features.empty()) {
        return fail(5, "cartography adapter did not produce complete admin0 features: " + result.diagnostic);
    }

    std::set<std::int64_t> used_colors;
    for (const auto& feature : result.features) {
        if (feature.properties.size() != 15U) {
            return fail(6, "cartography adapter property cardinality is not 15");
        }
        const auto* mapcolor7 = property(feature, "mapcolor7");
        const auto* iso_a2 = property(feature, "iso_a2");
        if (mapcolor7 == nullptr || iso_a2 == nullptr ||
            !std::holds_alternative<std::int64_t>(mapcolor7->value) ||
            !std::holds_alternative<std::string>(iso_a2->value)) {
            return fail(7, "mapcolor7/iso_a2 typed properties are absent");
        }
        const std::int64_t color = std::get<std::int64_t>(mapcolor7->value);
        if (color < 1 || color > 7) {
            return fail(8, "MAPCOLOR7 lies outside 1..7");
        }
        used_colors.insert(color);
    }
    if (used_colors.size() < 6U) {
        return fail(9, "pinned world does not exercise enough MAPCOLOR7 assignments");
    }

    std::cout
        << "natural_earth_cartography: PASS"
        << " features=" << result.features.size()
        << " palette_assignments=" << used_colors.size()
        << '\n';
    return EXIT_SUCCESS;
}
