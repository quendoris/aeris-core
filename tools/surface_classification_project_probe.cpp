// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/project/source_bridge.hpp"
#include "aeris/project/source_reader.hpp"
#include "aeris/source/natural_earth.hpp"
#include "aeris/source/registry.hpp"
#include "aeris/storage/project.hpp"
#include "aeris/storage/provenance.hpp"
#include "aeris/surface/classification.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

namespace {

constexpr std::string_view kProvider = "Natural Earth";
constexpr std::string_view kDataset = "ne_50m_antarctic_ice_shelves_polys";
constexpr std::string_view kSnapshot = "v5.1.2";
constexpr std::string_view kAdapterId =
    "natural-earth.ne-50m-antarctic-ice-shelves.surface-classification.v1";
constexpr std::string_view kProjectSourceId =
    "surface.antarctic-ice-shelves.natural-earth-50m";
constexpr std::string_view kSourceUri =
    "https://github.com/nvkelso/natural-earth-vector/tree/"
    "f1890d9f152c896d250a77557a5751a93d494776/50m_physical";
constexpr std::string_view kExpectedContentSha =
    "8b37e1c1612be041756a7062a6e5d12d7c130892bd26dcfc10c5be7884c7d281";

[[nodiscard]] aeris::source::SnapshotManifest manifest(
    const std::string_view retrieved_at_utc
) {
    aeris::source::SnapshotManifest value{};
    value.provider = std::string(kProvider);
    value.dataset = std::string(kDataset);
    value.snapshot = std::string(kSnapshot);
    value.source_uri = std::string(kSourceUri);
    value.retrieved_at_utc = std::string(retrieved_at_utc);
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

[[nodiscard]] aeris::source::SourceBinding binding() {
    aeris::source::SourceBinding value{};
    value.adapter_id = std::string(kAdapterId);
    value.capability = aeris::source::Capability::surface_classification;
    value.snapshot = std::string(kSnapshot);
    value.expected_content_sha256 = std::string(kExpectedContentSha);
    return value;
}

[[nodiscard]] bool is_floating_ice_shelf_feature(
    const aeris::source::Feature& feature
) {
    if (feature.rings.empty() || feature.properties.size() != 1U) return false;
    const aeris::source::FeatureProperty& property = feature.properties.front();
    if (property.key != aeris::surface::kSurfaceClassPropertyKey) return false;
    const auto* text = std::get_if<std::string>(&property.value);
    return text != nullptr &&
        aeris::surface::parse_surface_class_id(*text) ==
            aeris::surface::SurfaceClass::floating_ice_shelf;
}

[[nodiscard]] bool valid_semantic_result(const aeris::source::Result& result) {
    if (!result.ok() || !result.feature_properties_complete || result.features.empty()) {
        return false;
    }
    for (const aeris::source::Feature& feature : result.features) {
        if (!is_floating_ice_shelf_feature(feature)) return false;
    }
    return true;
}

int fail(const int code, const std::string& diagnostic) {
    std::cerr << "surface classification project proof failed: " << diagnostic << '\n';
    return code;
}

int import_mode(
    const std::filesystem::path& source_root,
    const std::filesystem::path& project_path,
    const std::string_view retrieved_at_utc
) {
    const aeris::source::SnapshotVerificationResult verified =
        aeris::source::verify_local_snapshot(source_root, manifest(retrieved_at_utc));
    if (!verified.ok() || !verified.snapshot.has_value()) {
        return fail(10, "verified snapshot rejected: " + verified.diagnostic);
    }
    if (verified.snapshot->content_sha256() != kExpectedContentSha) {
        return fail(11, "aggregate verified content identity differs from compatibility pin");
    }

    aeris::source::AdapterRegistry registry{};
    if (registry.add(
            std::make_unique<aeris::source::NaturalEarthAntarcticIceShelves50mAdapter>()
        ) != aeris::source::RegistryError::none) {
        return fail(12, "semantic adapter registration failed");
    }

    const aeris::source::RegistryLoadResult reference =
        registry.load(binding(), *verified.snapshot);
    if (!reference.ok() || !valid_semantic_result(reference.source)) {
        return fail(13, "semantic adapter did not produce canonical ice-shelf features");
    }

    std::error_code ec{};
    std::filesystem::remove(project_path, ec);
    ec.clear();
    std::filesystem::remove(std::filesystem::path(project_path.string() + ".session"), ec);
    if (!project_path.parent_path().empty()) {
        ec.clear();
        std::filesystem::create_directories(project_path.parent_path(), ec);
        if (ec) return fail(14, "could not create project directory: " + ec.message());
    }

    aeris::storage::ProjectCreateOptions create{};
    create.timestamp_utc = std::string(retrieved_at_utc);
    create.project_uuid = "718d7ce0-4d43-44bd-a0a9-51418fd7cb95";
    auto project = aeris::storage::ProjectStore::create(project_path, create);
    if (!project.ok()) return fail(15, "project creation failed: " + project.status.diagnostic);

    aeris::project::VerifiedSourceRecordRequest request{};
    request.source_id = std::string(kProjectSourceId);
    request.binding = binding();
    request.modified_utc = std::string(retrieved_at_utc);
    const aeris::project::SourceBridgeResult imported =
        aeris::project::record_verified_source_snapshot(
            *project.store,
            registry,
            *verified.snapshot,
            request
        );
    if (!imported.ok() || !imported.inserted || !imported.durably_committed) {
        return fail(16, "semantic source did not durably commit: " + imported.diagnostic);
    }
    const aeris::storage::Status integrity = project.store->verify_integrity();
    if (!integrity.ok()) {
        return fail(17, "project integrity failed immediately after semantic import: " +
            integrity.diagnostic);
    }

    const aeris::project::SourceBridgeResult retry =
        aeris::project::record_verified_source_snapshot(
            *project.store,
            registry,
            *verified.snapshot,
            request
        );
    if (!retry.ok() || retry.inserted || retry.durably_committed) {
        return fail(18, "exact semantic import retry was not idempotent");
    }

    std::cout << "surface_classification_project: IMPORT PASS"
              << " features=" << reference.source.features.size()
              << " content_sha256=" << kExpectedContentSha << '\n';
    return EXIT_SUCCESS;
}

int verify_mode(const std::filesystem::path& project_path) {
    auto project = aeris::storage::ProjectStore::open(project_path);
    if (!project.ok()) return fail(20, "project reopen failed: " + project.status.diagnostic);

    const aeris::storage::Status integrity = project.store->verify_integrity();
    if (!integrity.ok()) {
        return fail(21, "reopened project failed deep integrity verification: " +
            integrity.diagnostic);
    }

    const aeris::storage::SourceSnapshotListResult sources =
        aeris::storage::list_source_snapshots(*project.store);
    if (!sources.ok() || sources.records.size() != 1U) {
        return fail(22, "reopened project does not contain exactly one semantic source");
    }
    const aeris::storage::SourceSnapshotRecord& record = sources.records.front();
    if (record.source_id != kProjectSourceId || record.provider != kProvider ||
        record.dataset != kDataset || record.snapshot != kSnapshot ||
        record.content_sha256 != kExpectedContentSha) {
        return fail(23, "durable semantic provenance differs from pinned source identity");
    }

    const aeris::project::DurableSourceLoadResult loaded =
        aeris::project::load_durable_source_result(*project.store, kProjectSourceId);
    if (!loaded.ok()) {
        return fail(24, "durable semantic source could not rehydrate: " + loaded.diagnostic);
    }
    if (!valid_semantic_result(loaded.source)) {
        return fail(25, "rehydrated semantic source lost class properties or geometry");
    }
    if (loaded.source.provenance.content_sha256 != kExpectedContentSha ||
        loaded.source.provenance.dataset != kDataset ||
        loaded.source.provenance.dataset_version != "4.1.0") {
        return fail(26, "rehydrated semantic provenance is incomplete or changed");
    }

    std::cout << "surface_classification_project: REOPEN PASS"
              << " features=" << loaded.source.features.size()
              << " source_files_required=no\n";
    return EXIT_SUCCESS;
}

}  // namespace

int main(const int argc, char** argv) {
    if (argc >= 2 && std::string_view(argv[1]) == "import") {
        if (argc != 5) {
            return fail(2, "usage: import <snapshot-root> <project.aeris> <retrieved-at-utc>");
        }
        return import_mode(argv[2], argv[3], argv[4]);
    }
    if (argc >= 2 && std::string_view(argv[1]) == "verify") {
        if (argc != 3) return fail(3, "usage: verify <project.aeris>");
        return verify_mode(argv[2]);
    }
    return fail(4, "expected import or verify mode");
}
