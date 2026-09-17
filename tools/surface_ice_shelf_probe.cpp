// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/source/acquisition.hpp"
#include "aeris/source/natural_earth.hpp"
#include "aeris/surface/classification.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <variant>

namespace {

constexpr const char* kSnapshot = "v5.1.2";
constexpr const char* kExpectedContentSha256 =
    "8b37e1c1612be041756a7062a6e5d12d7c130892bd26dcfc10c5be7884c7d281";

[[nodiscard]] aeris::source::SnapshotManifest manifest(
    const std::string& retrieved_at_utc
) {
    aeris::source::SnapshotManifest value{};
    value.provider = "Natural Earth";
    value.dataset = "ne_50m_antarctic_ice_shelves_polys";
    value.snapshot = kSnapshot;
    value.source_uri =
        "https://github.com/nvkelso/natural-earth-vector/tree/"
        "f1890d9f152c896d250a77557a5751a93d494776/50m_physical";
    value.retrieved_at_utc = retrieved_at_utc;
    value.resources.push_back({
        "geometry.shp",
        "ne_50m_antarctic_ice_shelves_polys.shp",
        "05d06b075deb3e4119f0510788b03af7100f2c25b060d5cfdd126fb6817004db",
        83960U,
    });
    value.resources.push_back({
        "crs.prj",
        "ne_50m_antarctic_ice_shelves_polys.prj",
        "3259f0e55290a82b1350646f604e8a7ee1e2136c0320a40fad838ab40819fff8",
        147U,
    });
    value.resources.push_back({
        "dataset.version",
        "ne_50m_antarctic_ice_shelves_polys.VERSION.txt",
        "3b10b6ad566eadbcacadb33c591f1ec629593d6adf47442e56e0f61996829ef7",
        6U,
    });
    return value;
}

[[nodiscard]] bool canonical_floating_ice_shelf(
    const aeris::source::Feature& feature
) {
    if (feature.properties.size() != 1U) return false;
    const auto& property = feature.properties.front();
    if (property.key != aeris::surface::kSurfaceClassPropertyKey) return false;
    const auto* value = std::get_if<std::string>(&property.value);
    if (value == nullptr) return false;
    const auto parsed = aeris::surface::parse_surface_class_id(*value);
    return parsed.has_value() &&
        *parsed == aeris::surface::SurfaceClass::floating_ice_shelf;
}

}  // namespace

int main(const int argc, char** const argv) {
    if (argc != 3) {
        std::cerr
            << "usage: aeris_surface_ice_shelf_probe <source-root> <retrieved-at-utc>\n";
        return EXIT_FAILURE;
    }

    const std::filesystem::path root(argv[1]);
    const std::string retrieved_at_utc(argv[2]);
    const aeris::source::SnapshotVerificationResult verified =
        aeris::source::verify_local_snapshot(root, manifest(retrieved_at_utc));
    if (!verified.ok() || !verified.snapshot.has_value()) {
        std::cerr
            << "snapshot verification failed: " << verified.diagnostic << '\n';
        return EXIT_FAILURE;
    }
    if (verified.snapshot->content_sha256() != kExpectedContentSha256) {
        std::cerr
            << "aggregate content identity mismatch: "
            << verified.snapshot->content_sha256() << '\n';
        return EXIT_FAILURE;
    }

    const aeris::source::NaturalEarthAntarcticIceShelves50mAdapter adapter{};
    const aeris::source::Result result = adapter.load(
        *verified.snapshot,
        {
            aeris::source::Capability::surface_classification,
            kSnapshot,
            "",
        }
    );
    if (!result.ok()) {
        std::cerr << "adapter failed: " << result.diagnostic << '\n';
        return EXIT_FAILURE;
    }
    if (result.provenance.provider != "Natural Earth" ||
        result.provenance.dataset != "ne_50m_antarctic_ice_shelves_polys" ||
        result.provenance.snapshot != kSnapshot ||
        result.provenance.dataset_version != "4.1.0" ||
        result.provenance.content_sha256 != kExpectedContentSha256 ||
        !result.provenance.worldview.empty() ||
        !result.feature_properties_complete ||
        result.features.empty()) {
        std::cerr << "adapter provenance/classification contract mismatch\n";
        return EXIT_FAILURE;
    }

    std::size_t ring_count = 0U;
    std::size_t vertex_count = 0U;
    for (const aeris::source::Feature& feature : result.features) {
        if (!canonical_floating_ice_shelf(feature) || feature.rings.empty()) {
            std::cerr << "feature lacks canonical floating-ice-shelf semantics\n";
            return EXIT_FAILURE;
        }
        ring_count += feature.rings.size();
        for (const aeris::source::FeatureRing& ring : feature.rings) {
            if (ring.geometry.vertices.size() < 3U) {
                std::cerr << "feature contains degenerate canonical ring\n";
                return EXIT_FAILURE;
            }
            vertex_count += ring.geometry.vertices.size();
        }
    }

    std::cout
        << "surface_ice_shelf_probe: PASS"
        << " features=" << result.features.size()
        << " rings=" << ring_count
        << " vertices=" << vertex_count
        << " content_sha256=" << result.provenance.content_sha256
        << '\n';
    return EXIT_SUCCESS;
}
