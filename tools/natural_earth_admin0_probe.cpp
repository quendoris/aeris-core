// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/source/acquisition.hpp"
#include "aeris/source/natural_earth.hpp"
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
    std::cerr << "Natural Earth admin0 proof failed: " << diagnostic << '\n';
    return code;
}

bool add_resource(
    aeris::source::SnapshotManifest& manifest,
    const std::filesystem::path& root,
    std::string logical_name,
    std::filesystem::path relative_path) {
    const std::filesystem::path full = root / relative_path;
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(full, error);
    if (error) return false;
    const aeris::util::Sha256FileResult hash = aeris::util::sha256_file(full);
    if (!hash.ok()) return false;

    aeris::source::ResourceSpec resource{};
    resource.logical_name = std::move(logical_name);
    resource.relative_path = std::move(relative_path);
    resource.sha256 = hash.digest.hex();
    resource.size_bytes = size;
    manifest.resources.push_back(std::move(resource));
    return true;
}

const aeris::source::FeatureProperty* property(
    const aeris::source::Feature& feature,
    const std::string_view key) {
    for (const aeris::source::FeatureProperty& candidate : feature.properties) {
        if (candidate.key == key) return &candidate;
    }
    return nullptr;
}

}  // namespace

int main(const int argc, char** argv) {
    if (argc != 3) {
        return fail(2, "usage: <snapshot-root> <retrieved-at-utc>");
    }

    const std::filesystem::path root = argv[1];
    const std::string retrieved_at_utc = argv[2];

    aeris::source::SnapshotManifest manifest{};
    manifest.provider = std::string(kProvider);
    manifest.dataset = std::string(kDataset);
    manifest.snapshot = std::string(kSnapshot);
    manifest.source_uri = std::string(kSourceUri);
    manifest.retrieved_at_utc = retrieved_at_utc;

    if (!add_resource(manifest, root, "geometry.shp", "ne_110m_admin_0_countries.shp") ||
        !add_resource(manifest, root, "attributes.dbf", "ne_110m_admin_0_countries.dbf") ||
        !add_resource(manifest, root, "attributes.cpg", "ne_110m_admin_0_countries.cpg") ||
        !add_resource(manifest, root, "crs.prj", "ne_110m_admin_0_countries.prj") ||
        !add_resource(manifest, root, "dataset.version", "ne_110m_admin_0_countries.VERSION.txt")) {
        return fail(3, "could not hash all pinned admin0 resources");
    }

    aeris::source::SnapshotVerificationResult verified =
        aeris::source::verify_local_snapshot(root, manifest);
    if (!verified.ok() || !verified.snapshot.has_value()) {
        return fail(4, "verified snapshot construction failed: " + verified.diagnostic);
    }

    const aeris::source::NaturalEarthAdmin0Countries110mAdapter adapter{};
    aeris::source::Request request{};
    request.capability = aeris::source::Capability::admin0;
    request.snapshot = std::string(kSnapshot);
    request.worldview = "natural-earth.de-facto";
    const aeris::source::Result result = adapter.load(*verified.snapshot, request);
    if (!result.ok()) {
        return fail(5, "adapter load failed: " + result.diagnostic);
    }
    if (!result.feature_properties_complete || result.features.empty()) {
        return fail(6, "adapter did not produce a nonempty complete feature-property channel");
    }
    if (result.provenance.worldview != request.worldview) {
        return fail(7, "adapter provenance worldview differs from requested pinned worldview");
    }

    std::set<std::string> stable_ids;
    std::set<std::int64_t> ne_ids;
    std::size_t rings = 0U;
    std::size_t vertices = 0U;
    for (const aeris::source::Feature& feature : result.features) {
        if (!stable_ids.insert(feature.stable_id).second || feature.properties.size() != 14U) {
            return fail(8, "feature identity or complete property cardinality is malformed");
        }

        const auto* name = property(feature, "name");
        const auto* iso_a2 = property(feature, "iso_a2");
        const auto* adm0_a3 = property(feature, "adm0_a3");
        const auto* ne_id = property(feature, "ne_id");
        if (name == nullptr || iso_a2 == nullptr || adm0_a3 == nullptr || ne_id == nullptr ||
            !std::holds_alternative<std::string>(name->value) ||
            !std::holds_alternative<std::string>(iso_a2->value) ||
            !std::holds_alternative<std::string>(adm0_a3->value) ||
            !std::holds_alternative<std::int64_t>(ne_id->value)) {
            return fail(9, "required typed political properties are absent or have wrong types");
        }
        if (std::get<std::string>(name->value).empty() ||
            std::get<std::string>(adm0_a3->value).empty()) {
            return fail(10, "required country name/admin code is empty");
        }
        if (!ne_ids.insert(std::get<std::int64_t>(ne_id->value)).second) {
            return fail(11, "NE_ID is not unique across the adapter result");
        }

        rings += feature.rings.size();
        for (const aeris::source::FeatureRing& ring : feature.rings) {
            vertices += ring.geometry.vertices.size();
        }
    }

    std::cout
        << "natural_earth_admin0: PASS"
        << " features=" << result.features.size()
        << " rings=" << rings
        << " vertices=" << vertices
        << " content_sha256=" << verified.snapshot->content_sha256()
        << '\n';
    return EXIT_SUCCESS;
}
