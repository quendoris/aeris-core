// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/project/source_bridge.hpp"

#include "aeris/storage/dataset.hpp"

#include <cstdint>
#include <limits>
#include <utility>

namespace aeris::project {
namespace {

[[nodiscard]] SourceBridgeResult failure(
    const SourceBridgeError error,
    std::string diagnostic
) {
    SourceBridgeResult result{};
    result.error = error;
    result.diagnostic = std::move(diagnostic);
    return result;
}

[[nodiscard]] bool provenance_matches_verified_manifest(
    const source::Provenance& provenance,
    const source::SnapshotManifest& manifest,
    const source::VerifiedSnapshot& snapshot,
    const source::SourceBinding& binding
) noexcept {
    return binding.snapshot == manifest.snapshot &&
           provenance.provider == manifest.provider &&
           provenance.dataset == manifest.dataset &&
           provenance.snapshot == manifest.snapshot &&
           provenance.source_uri == manifest.source_uri &&
           provenance.retrieved_at_utc == manifest.retrieved_at_utc &&
           provenance.content_sha256 == snapshot.content_sha256();
}

[[nodiscard]] bool map_role(
    const source::RingRole role,
    storage::StoredRingRole& stored) noexcept {
    switch (role) {
        case source::RingRole::exterior:
            stored = storage::StoredRingRole::exterior;
            return true;
        case source::RingRole::interior:
            stored = storage::StoredRingRole::interior;
            return true;
    }
    return false;
}

[[nodiscard]] bool map_interior_side(
    const geometry::RingInteriorSide side,
    storage::StoredInteriorSide& stored) noexcept {
    switch (side) {
        case geometry::RingInteriorSide::unspecified:
            stored = storage::StoredInteriorSide::unspecified;
            return true;
        case geometry::RingInteriorSide::left:
            stored = storage::StoredInteriorSide::left;
            return true;
        case geometry::RingInteriorSide::right:
            stored = storage::StoredInteriorSide::right;
            return true;
    }
    return false;
}

[[nodiscard]] storage::StoredFeaturePropertyValue map_property_value(
    const source::FeaturePropertyValue& value) {
    return std::visit(
        [](const auto& item) -> storage::StoredFeaturePropertyValue {
            return item;
        },
        value);
}

}  // namespace

SourceBridgeResult record_verified_source_snapshot(
    storage::ProjectStore& project,
    const source::AdapterRegistry& registry,
    const source::VerifiedSnapshot& snapshot,
    const VerifiedSourceRecordRequest& request
) {
    if (request.source_id.empty() || request.source_id.find('\0') != std::string::npos ||
        !storage::is_canonical_utc_timestamp(request.modified_utc)) {
        return failure(
            SourceBridgeError::invalid_request,
            "project source ID must be non-empty/NUL-free and mutation time must be canonical Gregorian UTC"
        );
    }

    const source::RegistryLoadResult loaded = registry.load(request.binding, snapshot);
    if (!loaded.ok()) {
        SourceBridgeResult result = failure(
            SourceBridgeError::registry_rejected,
            loaded.diagnostic.empty() ? "source registry rejected verified snapshot" : loaded.diagnostic
        );
        result.registry_error = loaded.error;
        result.source_error = loaded.source_error;
        return result;
    }

    const source::Adapter* adapter = registry.find(request.binding.adapter_id);
    if (adapter == nullptr) {
        SourceBridgeResult result = failure(
            SourceBridgeError::registry_rejected,
            "source adapter disappeared after successful registry load"
        );
        result.registry_error = source::RegistryError::adapter_not_found;
        return result;
    }

    const source::SnapshotManifest& manifest = snapshot.manifest();
    const source::Provenance& provenance = loaded.source.provenance;
    if (!provenance_matches_verified_manifest(provenance, manifest, snapshot, request.binding)) {
        return failure(
            SourceBridgeError::verified_snapshot_mismatch,
            "adapter provenance does not describe the exact verified acquisition manifest"
        );
    }

    storage::SourceDatasetRecord dataset{};
    storage::SourceSnapshotRecord& record = dataset.source;
    record.source_id = request.source_id;
    const source::AdapterDescriptor descriptor = adapter->descriptor();
    record.adapter_id = std::string(descriptor.adapter_id);
    record.capability_bits = source::capability_bit(request.binding.capability);
    record.temporal_class = static_cast<std::uint8_t>(descriptor.temporal_class);
    record.provider = provenance.provider;
    record.dataset = provenance.dataset;
    record.snapshot = provenance.snapshot;
    record.dataset_version = provenance.dataset_version;
    record.source_uri = provenance.source_uri;
    record.license_id = provenance.license_id;
    record.content_sha256 = provenance.content_sha256;
    record.retrieved_at_utc = provenance.retrieved_at_utc;
    record.worldview = provenance.worldview;
    record.resources.reserve(manifest.resources.size());

    for (const source::ResourceSpec& resource : manifest.resources) {
        storage::SourceResourceRecord stored{};
        stored.logical_name = resource.logical_name;
        stored.sha256 = resource.sha256;
        if (resource.size_bytes.has_value()) {
            if (*resource.size_bytes >
                static_cast<std::uintmax_t>(std::numeric_limits<std::uint64_t>::max())) {
                return failure(
                    SourceBridgeError::resource_size_overflow,
                    "verified resource size exceeds the project format's uint64 range"
                );
            }
            stored.size_bytes = static_cast<std::uint64_t>(*resource.size_bytes);
        }
        record.resources.push_back(std::move(stored));
    }

    dataset.geometry.source_id = request.source_id;
    dataset.geometry.features.reserve(loaded.source.features.size());
    if (loaded.source.feature_properties_complete) {
        dataset.feature_properties.emplace();
        dataset.feature_properties->source_id = request.source_id;
        dataset.feature_properties->features.reserve(loaded.source.features.size());
    }

    for (const source::Feature& feature : loaded.source.features) {
        storage::FeatureGeometryRecord stored_feature{};
        stored_feature.stable_id = feature.stable_id;
        stored_feature.source_feature_id = feature.source_id;
        stored_feature.rings.reserve(feature.rings.size());

        for (const source::FeatureRing& ring : feature.rings) {
            storage::GeographicRingRecord stored_ring{};
            if (!map_role(ring.role, stored_ring.role) ||
                !map_interior_side(ring.geometry.interior_side, stored_ring.interior_side)) {
                return failure(
                    SourceBridgeError::invalid_request,
                    "adapter geographic ring contains an unsupported topology enum value"
                );
            }

            const auto winding = static_cast<long long>(ring.geometry.longitude_winding);
            if (winding < static_cast<long long>(std::numeric_limits<std::int32_t>::min()) ||
                winding > static_cast<long long>(std::numeric_limits<std::int32_t>::max())) {
                return failure(
                    SourceBridgeError::invalid_request,
                    "adapter geographic ring winding exceeds the project format int32 domain"
                );
            }

            stored_ring.longitude_winding = static_cast<std::int32_t>(winding);
            stored_ring.closing_longitude_rad = ring.geometry.closing_longitude_rad;
            stored_ring.vertices.reserve(ring.geometry.vertices.size());
            for (const geometry::GeodeticPoint point : ring.geometry.vertices) {
                stored_ring.vertices.push_back({point.longitude_rad, point.latitude_rad});
            }
            stored_feature.rings.push_back(std::move(stored_ring));
        }
        dataset.geometry.features.push_back(std::move(stored_feature));

        if (dataset.feature_properties.has_value()) {
            storage::FeaturePropertiesRecord stored_properties{};
            stored_properties.stable_id = feature.stable_id;
            stored_properties.properties.reserve(feature.properties.size());
            for (const source::FeatureProperty& property : feature.properties) {
                storage::StoredFeatureProperty stored_property{};
                stored_property.key = property.key;
                stored_property.value = map_property_value(property.value);
                stored_properties.properties.push_back(std::move(stored_property));
            }
            dataset.feature_properties->features.push_back(std::move(stored_properties));
        }
    }

    const storage::SourceDatasetMutationResult stored =
        storage::store_source_dataset(project, dataset, request.modified_utc);
    if (!stored.ok()) {
        SourceBridgeResult result = failure(
            SourceBridgeError::storage_rejected,
            stored.status.diagnostic.empty()
                ? "project storage rejected verified source dataset"
                : stored.status.diagnostic
        );
        result.storage_error = stored.status.error;
        result.inserted = stored.inserted;
        result.durably_committed = stored.durably_committed;
        return result;
    }

    SourceBridgeResult result{};
    result.inserted = stored.inserted;
    result.durably_committed = stored.durably_committed;
    return result;
}

}  // namespace aeris::project
