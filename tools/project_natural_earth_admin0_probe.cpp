// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/project/source_bridge.hpp"
#include "aeris/project/source_reader.hpp"
#include "aeris/source/natural_earth.hpp"
#include "aeris/storage/feature_property.hpp"
#include "aeris/storage/geometry.hpp"
#include "aeris/storage/provenance.hpp"
#include "aeris/util/sha256.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::string_view kProvider = "Natural Earth";
constexpr std::string_view kDataset = "ne_110m_admin_0_countries";
constexpr std::string_view kSnapshot = "v5.1.2";
constexpr std::string_view kAdapterId =
    "natural-earth.ne-110m-admin0-countries.shapefile-dbf.v1";
constexpr std::string_view kProjectSourceId = "world.admin0.natural-earth-110m";
constexpr std::string_view kWorldview = "natural-earth.de-facto";
constexpr std::string_view kSourceUri =
    "https://github.com/nvkelso/natural-earth-vector/tree/f1890d9f152c896d250a77557a5751a93d494776/110m_cultural";

int fail(const int code, const std::string& diagnostic) {
    std::cerr << "project Natural Earth admin0 proof failed: " << diagnostic << '\n';
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
    manifest.resources.push_back({
        std::move(logical_name), std::move(relative_path), hash.digest.hex(), size
    });
    return true;
}

bool build_manifest(
    const std::filesystem::path& root,
    const std::string& retrieved_at_utc,
    aeris::source::SnapshotManifest& manifest) {
    manifest.provider = std::string(kProvider);
    manifest.dataset = std::string(kDataset);
    manifest.snapshot = std::string(kSnapshot);
    manifest.source_uri = std::string(kSourceUri);
    manifest.retrieved_at_utc = retrieved_at_utc;
    return add_resource(manifest, root, "geometry.shp", "ne_110m_admin_0_countries.shp") &&
           add_resource(manifest, root, "attributes.dbf", "ne_110m_admin_0_countries.dbf") &&
           add_resource(manifest, root, "attributes.cpg", "ne_110m_admin_0_countries.cpg") &&
           add_resource(manifest, root, "crs.prj", "ne_110m_admin_0_countries.prj") &&
           add_resource(manifest, root, "dataset.version", "ne_110m_admin_0_countries.VERSION.txt");
}

bool same_property_value(
    const aeris::source::FeaturePropertyValue& source,
    const aeris::storage::StoredFeaturePropertyValue& stored) {
    if (source.index() != stored.index()) return false;
    switch (source.index()) {
        case 0U: return std::get<bool>(source) == std::get<bool>(stored);
        case 1U: return std::get<std::int64_t>(source) == std::get<std::int64_t>(stored);
        case 2U: return std::get<double>(source) == std::get<double>(stored);
        case 3U: return std::get<std::string>(source) == std::get<std::string>(stored);
        default: return false;
    }
}

bool compare_properties(
    const aeris::source::Feature& source,
    const aeris::storage::FeaturePropertiesLoadResult& stored) {
    if (!stored.ok() || source.properties.size() != stored.properties.size()) return false;
    std::map<std::string, const aeris::source::FeatureProperty*> expected;
    for (const auto& property : source.properties) expected.emplace(property.key, &property);
    if (expected.size() != source.properties.size()) return false;
    for (const auto& actual : stored.properties) {
        const auto found = expected.find(actual.key);
        if (found == expected.end() || !same_property_value(found->second->value, actual.value)) {
            return false;
        }
    }
    return true;
}

bool verify_stored_dataset(
    const aeris::source::Result& reference,
    aeris::storage::ProjectStore& project) {
    const auto geometry =
        aeris::storage::list_source_geometry_index(project, kProjectSourceId);
    const auto properties =
        aeris::storage::list_source_feature_properties_index(project, kProjectSourceId);
    if (!geometry.ok() || !properties.ok() ||
        geometry.features.size() != reference.features.size() ||
        properties.features.size() != reference.features.size()) {
        return false;
    }

    std::map<std::string, const aeris::source::Feature*> expected;
    for (const auto& feature : reference.features) expected.emplace(feature.stable_id, &feature);
    if (expected.size() != reference.features.size()) return false;

    for (const auto& entry : properties.features) {
        const auto found = expected.find(entry.stable_id);
        if (found == expected.end() || entry.source_feature_id != found->second->source_id ||
            entry.property_count != found->second->properties.size()) {
            return false;
        }
        const auto loaded = aeris::storage::load_feature_properties(
            project, kProjectSourceId, entry.stable_id);
        if (!compare_properties(*found->second, loaded)) return false;
    }
    return true;
}

bool same_provenance(
    const aeris::source::Provenance& left,
    const aeris::source::Provenance& right
) noexcept {
    return left.provider == right.provider &&
           left.dataset == right.dataset &&
           left.snapshot == right.snapshot &&
           left.dataset_version == right.dataset_version &&
           left.source_uri == right.source_uri &&
           left.license_id == right.license_id &&
           left.content_sha256 == right.content_sha256 &&
           left.retrieved_at_utc == right.retrieved_at_utc &&
           left.worldview == right.worldview;
}

bool same_source_feature(
    const aeris::source::Feature& left,
    const aeris::source::Feature& right
) {
    if (left.stable_id != right.stable_id ||
        left.source_id != right.source_id ||
        left.rings.size() != right.rings.size() ||
        left.properties.size() != right.properties.size()) {
        return false;
    }

    for (std::size_t ring_index = 0U; ring_index < left.rings.size(); ++ring_index) {
        const auto& a = left.rings[ring_index];
        const auto& b = right.rings[ring_index];
        if (a.role != b.role ||
            a.geometry.interior_side != b.geometry.interior_side ||
            a.geometry.longitude_winding != b.geometry.longitude_winding ||
            a.geometry.closing_longitude_rad != b.geometry.closing_longitude_rad ||
            a.geometry.vertices.size() != b.geometry.vertices.size()) {
            return false;
        }
        for (std::size_t point_index = 0U; point_index < a.geometry.vertices.size(); ++point_index) {
            if (a.geometry.vertices[point_index].longitude_rad !=
                    b.geometry.vertices[point_index].longitude_rad ||
                a.geometry.vertices[point_index].latitude_rad !=
                    b.geometry.vertices[point_index].latitude_rad) {
                return false;
            }
        }
    }

    std::map<std::string, aeris::source::FeaturePropertyValue> left_properties;
    std::map<std::string, aeris::source::FeaturePropertyValue> right_properties;
    for (const auto& property : left.properties) {
        if (!left_properties.emplace(property.key, property.value).second) return false;
    }
    for (const auto& property : right.properties) {
        if (!right_properties.emplace(property.key, property.value).second) return false;
    }
    return left_properties == right_properties;
}

bool compare_rehydrated(
    const aeris::source::Result& reference,
    const aeris::source::Result& rehydrated
) {
    if (!reference.ok() || !rehydrated.ok() ||
        !reference.feature_properties_complete ||
        !rehydrated.feature_properties_complete ||
        !same_provenance(reference.provenance, rehydrated.provenance) ||
        reference.features.size() != rehydrated.features.size()) {
        return false;
    }

    std::map<std::string, const aeris::source::Feature*> expected;
    std::map<std::string, const aeris::source::Feature*> actual;
    for (const auto& feature : reference.features) expected.emplace(feature.stable_id, &feature);
    for (const auto& feature : rehydrated.features) actual.emplace(feature.stable_id, &feature);
    if (expected.size() != reference.features.size() ||
        actual.size() != rehydrated.features.size() ||
        expected.size() != actual.size()) {
        return false;
    }
    for (const auto& [stable_id, feature] : expected) {
        const auto found = actual.find(stable_id);
        if (found == actual.end() || !same_source_feature(*feature, *found->second)) return false;
    }
    return true;
}

}  // namespace

int main(const int argc, char** argv) {
    if (argc != 4) {
        return fail(2, "usage: <snapshot-root> <project-path> <retrieved-at-utc>");
    }
    const std::filesystem::path root = argv[1];
    const std::filesystem::path project_path = argv[2];
    const std::string retrieved_at_utc = argv[3];

    aeris::source::SnapshotManifest manifest{};
    if (!build_manifest(root, retrieved_at_utc, manifest)) {
        return fail(3, "could not hash all pinned admin0 resources");
    }
    auto verified = aeris::source::verify_local_snapshot(root, manifest);
    if (!verified.ok() || !verified.snapshot.has_value()) {
        return fail(4, "verified snapshot construction failed: " + verified.diagnostic);
    }

    aeris::source::AdapterRegistry registry{};
    if (registry.add(std::make_unique<aeris::source::NaturalEarthAdmin0Countries110mAdapter>()) !=
        aeris::source::RegistryError::none) {
        return fail(5, "admin0 adapter registration failed");
    }
    aeris::source::SourceBinding binding{};
    binding.adapter_id = std::string(kAdapterId);
    binding.capability = aeris::source::Capability::admin0;
    binding.snapshot = std::string(kSnapshot);
    binding.worldview = std::string(kWorldview);
    binding.expected_content_sha256 = verified.snapshot->content_sha256();

    const auto reference = registry.load(binding, *verified.snapshot);
    if (!reference.ok() || !reference.source.feature_properties_complete ||
        reference.source.features.empty()) {
        return fail(6, "verified admin0 reference load failed: " + reference.diagnostic);
    }

    std::error_code ignored;
    std::filesystem::remove(project_path, ignored);
    std::filesystem::remove(std::filesystem::path(project_path.string() + ".session"), ignored);
    if (!project_path.parent_path().empty()) {
        std::error_code error;
        std::filesystem::create_directories(project_path.parent_path(), error);
        if (error) return fail(7, "could not create project parent directory: " + error.message());
    }

    aeris::storage::ProjectCreateOptions create{};
    create.timestamp_utc = retrieved_at_utc;
    create.project_uuid = "12345678-90ab-4cde-8fab-1234567890ab";
    auto project = aeris::storage::ProjectStore::create(project_path, create);
    if (!project.ok()) return fail(8, "project creation failed: " + project.status.diagnostic);

    aeris::project::VerifiedSourceRecordRequest request{};
    request.source_id = std::string(kProjectSourceId);
    request.binding = binding;
    request.modified_utc = retrieved_at_utc;
    const auto imported = aeris::project::record_verified_source_snapshot(
        *project.store, registry, *verified.snapshot, request);
    if (!imported.ok() || !imported.inserted || !imported.durably_committed ||
        project.store->metadata().revision != 1U) {
        return fail(9, "admin0 import was not one durable project revision: " + imported.diagnostic);
    }
    if (!verify_stored_dataset(reference.source, *project.store)) {
        return fail(10, "stored admin0 geometry/property indexes differ from adapter output");
    }

    project.store.reset();
    auto reopened = aeris::storage::ProjectStore::open(project_path);
    if (!reopened.ok()) return fail(11, "admin0 project did not reopen: " + reopened.status.diagnostic);
    if (!verify_stored_dataset(reference.source, *reopened.store)) {
        return fail(12, "reopened admin0 properties differ from adapter output");
    }
    const auto rehydrated = aeris::project::load_durable_source_result(
        *reopened.store, kProjectSourceId);
    if (!rehydrated.ok() || !compare_rehydrated(reference.source, rehydrated.source)) {
        return fail(13, "rehydrated admin0 Result differs from adapter output: " +
            rehydrated.diagnostic);
    }
    const auto integrity = reopened.store->verify_integrity();
    if (!integrity.ok()) return fail(14, "reopened admin0 project failed deep integrity: " + integrity.diagnostic);

    const auto retry = aeris::project::record_verified_source_snapshot(
        *reopened.store, registry, *verified.snapshot, request);
    if (!retry.ok() || retry.inserted || retry.durably_committed ||
        reopened.store->metadata().revision != 1U) {
        return fail(15, "exact admin0 retry was not idempotent");
    }

    std::cout
        << "project_natural_earth_admin0: PASS"
        << " features=" << reference.source.features.size()
        << " content_sha256=" << verified.snapshot->content_sha256()
        << " rehydrated=yes"
        << '\n';
    return EXIT_SUCCESS;
}
