// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/project/source_bridge.hpp"
#include "aeris/project/source_reader.hpp"
#include "aeris/source/natural_earth.hpp"
#include "aeris/storage/geometry.hpp"
#include "aeris/storage/provenance.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

constexpr std::string_view kProvider = "Natural Earth";
constexpr std::string_view kDataset = "ne_110m_land";
constexpr std::string_view kSnapshot = "v5.1.2";
constexpr std::string_view kAdapterId = "natural-earth.ne-110m-land.shapefile.v1";
constexpr std::string_view kProjectSourceId = "world.land.natural-earth-110m";
constexpr std::string_view kSourceUri =
    "https://github.com/nvkelso/natural-earth-vector/tree/f1890d9f152c896d250a77557a5751a93d494776/110m_physical";

[[nodiscard]] std::string trim_ascii(std::string value) {
    const std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const std::size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1U);
}

[[nodiscard]] bool parse_size(const std::string_view text, std::uintmax_t& value) noexcept {
    if (text.empty()) return false;
    const char* const first = text.data();
    const char* const last = text.data() + text.size();
    const auto parsed = std::from_chars(first, last, value);
    return parsed.ec == std::errc{} && parsed.ptr == last;
}

[[nodiscard]] bool split_resource_line(
    const std::string& line,
    std::string& logical_name,
    std::string& relative_path,
    std::string& sha256,
    std::uintmax_t& size_bytes) {
    const std::size_t first = line.find('\t');
    if (first == std::string::npos) return false;
    const std::size_t second = line.find('\t', first + 1U);
    if (second == std::string::npos) return false;
    const std::size_t third = line.find('\t', second + 1U);
    if (third == std::string::npos || line.find('\t', third + 1U) != std::string::npos) return false;

    logical_name = line.substr(0U, first);
    relative_path = line.substr(first + 1U, second - first - 1U);
    sha256 = line.substr(second + 1U, third - second - 1U);
    const std::string_view size_text(line.data() + third + 1U, line.size() - third - 1U);
    return !logical_name.empty() && !relative_path.empty() && !sha256.empty() &&
           parse_size(size_text, size_bytes);
}

[[nodiscard]] bool load_pin_resources(
    const std::filesystem::path& pin_directory,
    aeris::source::SnapshotManifest& manifest) {
    std::ifstream input(pin_directory / "resources.tsv", std::ios::binary);
    if (!input) return false;

    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line.front() == '#') continue;

        std::string logical_name;
        std::string relative_path;
        std::string sha256;
        std::uintmax_t size_bytes = 0U;
        if (!split_resource_line(line, logical_name, relative_path, sha256, size_bytes)) return false;

        aeris::source::ResourceSpec resource{};
        resource.logical_name = std::move(logical_name);
        resource.relative_path = std::filesystem::path(std::move(relative_path));
        resource.sha256 = std::move(sha256);
        resource.size_bytes = size_bytes;
        manifest.resources.push_back(std::move(resource));
    }
    return input.eof() && !manifest.resources.empty();
}

[[nodiscard]] std::string load_expected_content_hash(const std::filesystem::path& pin_directory) {
    std::ifstream input(pin_directory / "content.sha256", std::ios::binary);
    if (!input) return {};
    std::string value;
    std::getline(input, value);
    if (!input && !input.eof()) return {};
    return trim_ascii(std::move(value));
}

[[nodiscard]] bool role_matches(
    const aeris::source::RingRole source,
    const aeris::storage::StoredRingRole stored) noexcept {
    switch (source) {
        case aeris::source::RingRole::exterior:
            return stored == aeris::storage::StoredRingRole::exterior;
        case aeris::source::RingRole::interior:
            return stored == aeris::storage::StoredRingRole::interior;
    }
    return false;
}

[[nodiscard]] bool side_matches(
    const aeris::geometry::RingInteriorSide source,
    const aeris::storage::StoredInteriorSide stored) noexcept {
    switch (source) {
        case aeris::geometry::RingInteriorSide::unspecified:
            return stored == aeris::storage::StoredInteriorSide::unspecified;
        case aeris::geometry::RingInteriorSide::left:
            return stored == aeris::storage::StoredInteriorSide::left;
        case aeris::geometry::RingInteriorSide::right:
            return stored == aeris::storage::StoredInteriorSide::right;
    }
    return false;
}

[[nodiscard]] bool compare_feature(
    const aeris::source::Feature& source,
    const aeris::storage::FeatureGeometryRecord& stored) {
    if (source.stable_id != stored.stable_id || source.source_id != stored.source_feature_id ||
        source.rings.size() != stored.rings.size()) {
        return false;
    }

    for (std::size_t ring_index = 0U; ring_index < source.rings.size(); ++ring_index) {
        const aeris::source::FeatureRing& source_ring = source.rings[ring_index];
        const aeris::storage::GeographicRingRecord& stored_ring = stored.rings[ring_index];
        if (!role_matches(source_ring.role, stored_ring.role) ||
            !side_matches(source_ring.geometry.interior_side, stored_ring.interior_side) ||
            source_ring.geometry.closing_longitude_rad != stored_ring.closing_longitude_rad ||
            source_ring.geometry.longitude_winding != stored_ring.longitude_winding ||
            source_ring.geometry.vertices.size() != stored_ring.vertices.size()) {
            return false;
        }

        for (std::size_t point_index = 0U;
             point_index < source_ring.geometry.vertices.size();
             ++point_index) {
            const aeris::geometry::GeodeticPoint& a = source_ring.geometry.vertices[point_index];
            const aeris::storage::GeographicPointRecord& b = stored_ring.vertices[point_index];
            if (a.longitude_rad != b.longitude_rad || a.latitude_rad != b.latitude_rad) return false;
        }
    }
    return true;
}

[[nodiscard]] bool compare_all_features(
    const aeris::source::Result& reference,
    aeris::storage::ProjectStore& project) {
    const aeris::storage::SourceGeometryIndexResult index =
        aeris::storage::list_source_geometry_index(project, kProjectSourceId);
    if (!index.ok() || reference.features.size() != index.features.size()) return false;

    std::vector<const aeris::source::Feature*> ordered;
    ordered.reserve(reference.features.size());
    for (const aeris::source::Feature& feature : reference.features) ordered.push_back(&feature);
    std::sort(ordered.begin(), ordered.end(), [](const auto* a, const auto* b) {
        return a->stable_id < b->stable_id;
    });

    for (std::size_t position = 0U; position < ordered.size(); ++position) {
        const aeris::source::Feature& expected = *ordered[position];
        const aeris::storage::FeatureGeometryIndexEntry& entry = index.features[position];
        if (entry.stable_id != expected.stable_id ||
            entry.source_feature_id != expected.source_id ||
            entry.ring_count != expected.rings.size()) {
            return false;
        }

        const aeris::storage::FeatureGeometryLoadResult loaded =
            aeris::storage::load_feature_geometry(project, kProjectSourceId, entry.stable_id);
        if (!loaded.ok() || !compare_feature(expected, *loaded.feature)) {
            std::cerr << "feature mismatch after project reopen: " << expected.stable_id << '\n';
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool same_provenance(
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

[[nodiscard]] bool same_source_feature(
    const aeris::source::Feature& left,
    const aeris::source::Feature& right
) noexcept {
    if (left.stable_id != right.stable_id || left.source_id != right.source_id ||
        left.rings.size() != right.rings.size() ||
        !left.properties.empty() || !right.properties.empty()) {
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
    return true;
}

[[nodiscard]] bool compare_rehydrated(
    const aeris::source::Result& reference,
    const aeris::source::Result& rehydrated
) {
    if (!reference.ok() || !rehydrated.ok() ||
        !same_provenance(reference.provenance, rehydrated.provenance) ||
        reference.feature_properties_complete || rehydrated.feature_properties_complete ||
        reference.features.size() != rehydrated.features.size()) {
        return false;
    }

    std::vector<const aeris::source::Feature*> expected;
    std::vector<const aeris::source::Feature*> actual;
    expected.reserve(reference.features.size());
    actual.reserve(rehydrated.features.size());
    for (const auto& feature : reference.features) expected.push_back(&feature);
    for (const auto& feature : rehydrated.features) actual.push_back(&feature);
    const auto by_id = [](const auto* a, const auto* b) { return a->stable_id < b->stable_id; };
    std::sort(expected.begin(), expected.end(), by_id);
    std::sort(actual.begin(), actual.end(), by_id);
    for (std::size_t index = 0U; index < expected.size(); ++index) {
        if (!same_source_feature(*expected[index], *actual[index])) return false;
    }
    return true;
}

struct GeometryCounts final {
    std::size_t features = 0U;
    std::size_t rings = 0U;
    std::size_t vertices = 0U;
    std::size_t winding_rings = 0U;
};

[[nodiscard]] GeometryCounts count_geometry(const aeris::source::Result& source) noexcept {
    GeometryCounts counts{};
    counts.features = source.features.size();
    for (const aeris::source::Feature& feature : source.features) {
        counts.rings += feature.rings.size();
        for (const aeris::source::FeatureRing& ring : feature.rings) {
            counts.vertices += ring.geometry.vertices.size();
            if (ring.geometry.longitude_winding != 0) ++counts.winding_rings;
        }
    }
    return counts;
}

int fail(const int code, const std::string& diagnostic) {
    std::cerr << "project Natural Earth proof failed: " << diagnostic << '\n';
    return code;
}

}  // namespace

int main(const int argc, char** argv) {
    if (argc != 5) {
        return fail(2, "usage: <snapshot-root> <pin-directory> <project-path> <retrieved-at-utc>");
    }

    const std::filesystem::path snapshot_root = argv[1];
    const std::filesystem::path pin_directory = argv[2];
    const std::filesystem::path project_path = argv[3];
    const std::string retrieved_at_utc = argv[4];

    aeris::source::SnapshotManifest manifest{};
    manifest.provider = std::string(kProvider);
    manifest.dataset = std::string(kDataset);
    manifest.snapshot = std::string(kSnapshot);
    manifest.source_uri = std::string(kSourceUri);
    manifest.retrieved_at_utc = retrieved_at_utc;
    if (!load_pin_resources(pin_directory, manifest)) return fail(3, "could not load pinned resources.tsv");

    const std::string expected_content_hash = load_expected_content_hash(pin_directory);
    if (!aeris::storage::is_canonical_sha256(expected_content_hash)) {
        return fail(4, "pinned aggregate content SHA-256 is missing or noncanonical");
    }

    aeris::source::SnapshotVerificationResult verified =
        aeris::source::verify_local_snapshot(snapshot_root, manifest);
    if (!verified.ok() || !verified.snapshot.has_value()) {
        return fail(5, "pinned Natural Earth bytes did not pass source verification: " + verified.diagnostic);
    }
    if (verified.snapshot->content_sha256() != expected_content_hash) {
        return fail(6, "verified aggregate content hash differs from committed compatibility pin");
    }

    aeris::source::AdapterRegistry registry{};
    if (registry.add(std::make_unique<aeris::source::NaturalEarthLand110mAdapter>()) !=
        aeris::source::RegistryError::none) {
        return fail(7, "Natural Earth adapter registration failed");
    }

    aeris::source::SourceBinding binding{};
    binding.adapter_id = std::string(kAdapterId);
    binding.capability = aeris::source::Capability::land;
    binding.snapshot = std::string(kSnapshot);
    binding.expected_content_sha256 = expected_content_hash;

    const aeris::source::RegistryLoadResult reference = registry.load(binding, *verified.snapshot);
    if (!reference.ok()) {
        return fail(8, "Natural Earth reference adapter load failed: " + reference.diagnostic);
    }
    const GeometryCounts counts = count_geometry(reference.source);
    if (counts.features == 0U || counts.rings == 0U || counts.vertices == 0U) {
        return fail(9, "Natural Earth reference adapter produced empty geography");
    }

    std::error_code ec{};
    std::filesystem::remove(project_path, ec);
    ec.clear();
    std::filesystem::remove(std::filesystem::path(project_path.string() + ".session"), ec);
    if (!project_path.parent_path().empty()) {
        ec.clear();
        std::filesystem::create_directories(project_path.parent_path(), ec);
        if (ec) return fail(10, "could not create project parent directory: " + ec.message());
    }

    aeris::storage::ProjectCreateOptions create{};
    create.timestamp_utc = retrieved_at_utc;
    create.project_uuid = "01234567-89ab-4cde-8fab-0123456789ab";
    auto project = aeris::storage::ProjectStore::create(project_path, create);
    if (!project.ok()) return fail(11, "project creation failed: " + project.status.diagnostic);

    aeris::project::VerifiedSourceRecordRequest request{};
    request.source_id = std::string(kProjectSourceId);
    request.binding = binding;
    request.modified_utc = retrieved_at_utc;

    const aeris::project::SourceBridgeResult imported =
        aeris::project::record_verified_source_snapshot(
            *project.store,
            registry,
            *verified.snapshot,
            request);
    if (!imported.ok() || !imported.inserted || !imported.durably_committed) {
        return fail(12, "verified Natural Earth project import did not commit: " + imported.diagnostic);
    }
    if (project.store->metadata().revision != 1U) {
        return fail(13, "real-world dataset import did not advance exactly one project revision");
    }

    project.store.reset();
    auto reopened = aeris::storage::ProjectStore::open(project_path);
    if (!reopened.ok()) {
        return fail(14, "project did not reopen after Natural Earth import: " + reopened.status.diagnostic);
    }

    const aeris::storage::SourceSnapshotListResult sources =
        aeris::storage::list_source_snapshots(*reopened.store);
    if (!sources.ok() || sources.records.size() != 1U) {
        return fail(15, "reopened project does not contain exactly one source record");
    }
    if (sources.records.front().source_id != kProjectSourceId ||
        sources.records.front().content_sha256 != expected_content_hash ||
        sources.records.front().resources.size() != manifest.resources.size()) {
        return fail(16, "reopened source provenance differs from verified compatibility pin");
    }

    if (!compare_all_features(reference.source, *reopened.store)) {
        return fail(17, "stored Natural Earth geography differs from canonical adapter output");
    }
    const auto rehydrated = aeris::project::load_durable_source_result(
        *reopened.store, kProjectSourceId);
    if (!rehydrated.ok() || !compare_rehydrated(reference.source, rehydrated.source)) {
        return fail(18, "rehydrated Natural Earth land Result differs from adapter output: " +
            rehydrated.diagnostic);
    }
    const aeris::storage::Status integrity = reopened.store->verify_integrity();
    if (!integrity.ok()) {
        return fail(19, "reopened real-world project failed deep integrity verification: " + integrity.diagnostic);
    }

    const aeris::project::SourceBridgeResult retry =
        aeris::project::record_verified_source_snapshot(
            *reopened.store,
            registry,
            *verified.snapshot,
            request);
    if (!retry.ok() || retry.inserted || retry.durably_committed ||
        reopened.store->metadata().revision != 1U) {
        return fail(20, "exact real-world retry was not idempotent");
    }

    std::cout
        << "project_natural_earth: PASS"
        << " features=" << counts.features
        << " rings=" << counts.rings
        << " vertices=" << counts.vertices
        << " winding_rings=" << counts.winding_rings
        << " content_sha256=" << expected_content_hash
        << " rehydrated=yes"
        << '\n';
    return EXIT_SUCCESS;
}
