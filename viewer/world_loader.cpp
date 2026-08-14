// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "world_loader.hpp"

#include "aeris/source/acquisition.hpp"
#include "aeris/source/natural_earth.hpp"
#include "aeris/source/registry.hpp"

#include <memory>
#include <utility>

namespace aeris::viewer {
namespace {

constexpr const char* kExpectedContentSha256 =
    "5a9d2b70be942d7d0602ef299afe0ef039463831ade478aae11091f8c202cf6e";

[[nodiscard]] source::SnapshotManifest demo_manifest(std::string retrieved_at_utc) {
    source::SnapshotManifest manifest{};
    manifest.provider = "Natural Earth";
    manifest.dataset = "ne_110m_land";
    manifest.snapshot = "v5.1.2";
    manifest.source_uri =
        "https://github.com/nvkelso/natural-earth-vector/tree/"
        "f1890d9f152c896d250a77557a5751a93d494776/110m_physical";
    manifest.retrieved_at_utc = std::move(retrieved_at_utc);

    source::ResourceSpec geometry{};
    geometry.logical_name = "geometry.shp";
    geometry.relative_path = "ne_110m_land.shp";
    geometry.sha256 =
        "8689e6932b8e370e2ca4587cf3ba21e460b1235db37b6ed3c172c35b4a6088de";
    geometry.size_bytes = 89504U;
    manifest.resources.push_back(std::move(geometry));

    source::ResourceSpec projection{};
    projection.logical_name = "crs.prj";
    projection.relative_path = "ne_110m_land.prj";
    projection.sha256 =
        "3259f0e55290a82b1350646f604e8a7ee1e2136c0320a40fad838ab40819fff8";
    projection.size_bytes = 147U;
    manifest.resources.push_back(std::move(projection));

    source::ResourceSpec version{};
    version.logical_name = "dataset.version";
    version.relative_path = "ne_110m_land.VERSION.txt";
    version.sha256 =
        "3b10b6ad566eadbcacadb33c591f1ec629593d6adf47442e56e0f61996829ef7";
    version.size_bytes = 6U;
    manifest.resources.push_back(std::move(version));

    return manifest;
}

}  // namespace

WorldLoadResult load_pinned_demo_world(
    const std::filesystem::path& snapshot_root,
    std::string retrieved_at_utc
) {
    WorldLoadResult output{};

    auto verified = source::verify_local_snapshot(
        snapshot_root,
        demo_manifest(std::move(retrieved_at_utc))
    );
    if (!verified.ok()) {
        output.diagnostic =
            "Pinned snapshot verification failed (error " +
            std::to_string(static_cast<unsigned>(verified.error)) + ")";
        if (!verified.failed_resource.empty()) {
            output.diagnostic += ": " + verified.failed_resource;
        }
        if (!verified.diagnostic.empty()) {
            output.diagnostic += " — " + verified.diagnostic;
        }
        return output;
    }

    if (verified.snapshot->content_sha256() != kExpectedContentSha256) {
        output.diagnostic = "Pinned snapshot aggregate content identity mismatch";
        return output;
    }

    source::AdapterRegistry registry{};
    const auto added = registry.add(
        std::make_unique<source::NaturalEarthLand110mAdapter>()
    );
    if (added != source::RegistryError::none) {
        output.diagnostic = "Unable to register Natural Earth adapter";
        return output;
    }

    source::SourceBinding binding{};
    binding.adapter_id = "natural-earth.ne-110m-land.shapefile.v1";
    binding.capability = source::Capability::land;
    binding.snapshot = "v5.1.2";
    binding.expected_content_sha256 = kExpectedContentSha256;

    auto loaded = registry.load(binding, *verified.snapshot);
    if (!loaded.ok()) {
        output.diagnostic =
            "Natural Earth adapter load failed (registry " +
            std::to_string(static_cast<unsigned>(loaded.error)) +
            ", source " +
            std::to_string(static_cast<unsigned>(loaded.source_error)) + ")";
        if (!loaded.diagnostic.empty()) {
            output.diagnostic += " — " + loaded.diagnostic;
        }
        return output;
    }

    if (loaded.source.features.size() != 127U) {
        output.diagnostic = "Pinned demo world cardinality changed unexpectedly";
        return output;
    }

    output.world = std::make_shared<const source::Result>(
        std::move(loaded.source)
    );
    return output;
}

}  // namespace aeris::viewer
