// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "world_loader.hpp"

#include "aeris/source/acquisition.hpp"
#include "aeris/source/natural_earth.hpp"
#include "aeris/source/registry.hpp"
#include "aeris/util/sha256.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace aeris::viewer {
namespace {

constexpr const char* kExpectedLandContentSha256 =
    "5a9d2b70be942d7d0602ef299afe0ef039463831ade478aae11091f8c202cf6e";
constexpr const char* kExpectedAdmin0ContentSha256 =
    "2d971b3c627462cb22fdcd1468a8972b2a66677585fabcaa520bc6937ef47fb0";

[[nodiscard]] source::SnapshotManifest land_manifest(std::string retrieved_at_utc) {
    source::SnapshotManifest manifest{};
    manifest.provider = "Natural Earth";
    manifest.dataset = "ne_110m_land";
    manifest.snapshot = "v5.1.2";
    manifest.source_uri =
        "https://github.com/nvkelso/natural-earth-vector/tree/"
        "f1890d9f152c896d250a77557a5751a93d494776/110m_physical";
    manifest.retrieved_at_utc = std::move(retrieved_at_utc);

    manifest.resources.push_back({
        "geometry.shp",
        "ne_110m_land.shp",
        "8689e6932b8e370e2ca4587cf3ba21e460b1235db37b6ed3c172c35b4a6088de",
        89504U,
    });
    manifest.resources.push_back({
        "crs.prj",
        "ne_110m_land.prj",
        "3259f0e55290a82b1350646f604e8a7ee1e2136c0320a40fad838ab40819fff8",
        147U,
    });
    manifest.resources.push_back({
        "dataset.version",
        "ne_110m_land.VERSION.txt",
        "3b10b6ad566eadbcacadb33c591f1ec629593d6adf47442e56e0f61996829ef7",
        6U,
    });
    return manifest;
}

[[nodiscard]] bool add_local_resource(
    source::SnapshotManifest& manifest,
    const std::filesystem::path& root,
    std::string logical_name,
    std::filesystem::path relative_path,
    std::string& diagnostic
) {
    const std::filesystem::path path = root / relative_path;
    std::error_code size_error;
    const std::uintmax_t size = std::filesystem::file_size(path, size_error);
    if (size_error) {
        diagnostic = "could not inspect pinned political resource " +
            relative_path.string() + ": " + size_error.message();
        return false;
    }
    const util::Sha256FileResult hash = util::sha256_file(path);
    if (!hash.ok()) {
        diagnostic = "could not hash pinned political resource " + relative_path.string();
        return false;
    }
    manifest.resources.push_back({
        std::move(logical_name),
        std::move(relative_path),
        hash.digest.hex(),
        size,
    });
    return true;
}

[[nodiscard]] std::optional<source::SnapshotManifest> admin0_manifest(
    const std::filesystem::path& root,
    std::string retrieved_at_utc,
    std::string& diagnostic
) {
    source::SnapshotManifest manifest{};
    manifest.provider = "Natural Earth";
    manifest.dataset = "ne_110m_admin_0_countries";
    manifest.snapshot = "v5.1.2";
    manifest.source_uri =
        "https://github.com/nvkelso/natural-earth-vector/tree/"
        "f1890d9f152c896d250a77557a5751a93d494776/110m_cultural";
    manifest.retrieved_at_utc = std::move(retrieved_at_utc);

    if (!add_local_resource(manifest, root, "geometry.shp", "ne_110m_admin_0_countries.shp", diagnostic) ||
        !add_local_resource(manifest, root, "attributes.dbf", "ne_110m_admin_0_countries.dbf", diagnostic) ||
        !add_local_resource(manifest, root, "attributes.cpg", "ne_110m_admin_0_countries.cpg", diagnostic) ||
        !add_local_resource(manifest, root, "crs.prj", "ne_110m_admin_0_countries.prj", diagnostic) ||
        !add_local_resource(manifest, root, "dataset.version", "ne_110m_admin_0_countries.VERSION.txt", diagnostic)) {
        return std::nullopt;
    }
    return manifest;
}

[[nodiscard]] WorldLoadResult load_with_binding(
    const std::filesystem::path& snapshot_root,
    source::SnapshotManifest manifest,
    std::string expected_content_sha256,
    std::unique_ptr<source::Adapter> adapter,
    source::SourceBinding binding,
    const std::size_t expected_features
) {
    WorldLoadResult output{};
    auto verified = source::verify_local_snapshot(snapshot_root, manifest);
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

    if (verified.snapshot->content_sha256() != expected_content_sha256) {
        output.diagnostic = "Pinned snapshot aggregate content identity mismatch";
        return output;
    }

    source::AdapterRegistry registry{};
    if (registry.add(std::move(adapter)) != source::RegistryError::none) {
        output.diagnostic = "Unable to register Natural Earth adapter";
        return output;
    }

    binding.expected_content_sha256 = std::move(expected_content_sha256);
    auto loaded = registry.load(binding, *verified.snapshot);
    if (!loaded.ok()) {
        output.diagnostic =
            "Natural Earth adapter load failed (registry " +
            std::to_string(static_cast<unsigned>(loaded.error)) +
            ", source " +
            std::to_string(static_cast<unsigned>(loaded.source_error)) + ")";
        if (!loaded.diagnostic.empty()) output.diagnostic += " — " + loaded.diagnostic;
        return output;
    }

    if (loaded.source.features.size() != expected_features) {
        output.diagnostic = "Pinned demo world cardinality changed unexpectedly";
        return output;
    }

    output.world = std::make_shared<const source::Result>(std::move(loaded.source));
    return output;
}

}  // namespace

WorldLoadResult load_pinned_demo_world(
    const std::filesystem::path& snapshot_root,
    std::string retrieved_at_utc
) {
    source::SourceBinding binding{};
    binding.adapter_id = "natural-earth.ne-110m-land.shapefile.v1";
    binding.capability = source::Capability::land;
    binding.snapshot = "v5.1.2";
    return load_with_binding(
        snapshot_root,
        land_manifest(std::move(retrieved_at_utc)),
        kExpectedLandContentSha256,
        std::make_unique<source::NaturalEarthLand110mAdapter>(),
        std::move(binding),
        127U
    );
}

WorldLoadResult load_pinned_political_world(
    const std::filesystem::path& snapshot_root,
    std::string retrieved_at_utc
) {
    WorldLoadResult output{};
    std::string diagnostic;
    auto manifest = admin0_manifest(snapshot_root, std::move(retrieved_at_utc), diagnostic);
    if (!manifest.has_value()) {
        output.diagnostic = std::move(diagnostic);
        return output;
    }

    source::SourceBinding binding{};
    binding.adapter_id = "natural-earth.ne-110m-admin0-countries.shapefile-dbf.v1";
    binding.capability = source::Capability::admin0;
    binding.snapshot = "v5.1.2";
    binding.worldview = "natural-earth.de-facto";
    return load_with_binding(
        snapshot_root,
        std::move(*manifest),
        kExpectedAdmin0ContentSha256,
        std::make_unique<source::NaturalEarthAdmin0Countries110mAdapter>(),
        std::move(binding),
        177U
    );
}

WorkbenchWorlds load_pinned_workbench_worlds(
    const std::filesystem::path& snapshot_root,
    std::string retrieved_at_utc
) {
    WorkbenchWorlds output{};
    auto physical = load_pinned_demo_world(snapshot_root, retrieved_at_utc);
    if (!physical.ok()) {
        output.diagnostic = "Physical world: " + physical.diagnostic;
        return output;
    }
    auto political = load_pinned_political_world(snapshot_root, std::move(retrieved_at_utc));
    if (!political.ok()) {
        output.diagnostic = "Political world: " + political.diagnostic;
        return output;
    }
    output.physical = std::move(physical.world);
    output.political = std::move(political.world);
    return output;
}

const char* map_content_name(const MapContent content) noexcept {
    return content == MapContent::political ? "Political" : "Physical";
}

}  // namespace aeris::viewer
