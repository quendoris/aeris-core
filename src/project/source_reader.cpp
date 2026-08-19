// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/project/source_reader.hpp"

#include "aeris/storage/feature_property.hpp"
#include "aeris/storage/geometry.hpp"
#include "aeris/storage/provenance.hpp"

#include <algorithm>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace aeris::project {
namespace {

using FeatureKey = std::pair<std::string, std::string>;

[[nodiscard]] DurableSourceLoadResult failure(
    const DurableSourceLoadError error,
    std::string diagnostic,
    const storage::StorageError storage_error = storage::StorageError::none
) {
    DurableSourceLoadResult result{};
    result.error = error;
    result.storage_error = storage_error;
    result.diagnostic = std::move(diagnostic);
    result.source.error = source::SourceError::malformed_source;
    result.source.diagnostic = result.diagnostic;
    return result;
}

[[nodiscard]] DurableSourceLoadResult storage_failure(
    const storage::Status& status,
    std::string prefix
) {
    if (!status.diagnostic.empty()) {
        prefix += ": ";
        prefix += status.diagnostic;
    }
    return failure(
        DurableSourceLoadError::storage_rejected,
        std::move(prefix),
        status.error
    );
}

[[nodiscard]] const storage::SourceSnapshotRecord* find_source(
    const std::vector<storage::SourceSnapshotRecord>& records,
    const std::string_view source_id
) noexcept {
    const auto found = std::find_if(
        records.begin(), records.end(),
        [&](const storage::SourceSnapshotRecord& record) {
            return record.source_id == source_id;
        }
    );
    return found == records.end() ? nullptr : &*found;
}

[[nodiscard]] bool map_role(
    const storage::StoredRingRole stored,
    source::RingRole& role
) noexcept {
    switch (stored) {
        case storage::StoredRingRole::exterior:
            role = source::RingRole::exterior;
            return true;
        case storage::StoredRingRole::interior:
            role = source::RingRole::interior;
            return true;
    }
    return false;
}

[[nodiscard]] bool map_interior_side(
    const storage::StoredInteriorSide stored,
    geometry::RingInteriorSide& side
) noexcept {
    switch (stored) {
        case storage::StoredInteriorSide::unspecified:
            side = geometry::RingInteriorSide::unspecified;
            return true;
        case storage::StoredInteriorSide::left:
            side = geometry::RingInteriorSide::left;
            return true;
        case storage::StoredInteriorSide::right:
            side = geometry::RingInteriorSide::right;
            return true;
    }
    return false;
}

[[nodiscard]] source::FeaturePropertyValue map_property_value(
    const storage::StoredFeaturePropertyValue& value
) {
    return std::visit(
        [](const auto& item) -> source::FeaturePropertyValue {
            return item;
        },
        value
    );
}

[[nodiscard]] source::Provenance rehydrate_provenance(
    const storage::SourceSnapshotRecord& record
) {
    source::Provenance provenance{};
    provenance.provider = record.provider;
    provenance.dataset = record.dataset;
    provenance.snapshot = record.snapshot;
    provenance.dataset_version = record.dataset_version;
    provenance.source_uri = record.source_uri;
    provenance.license_id = record.license_id;
    provenance.content_sha256 = record.content_sha256;
    provenance.retrieved_at_utc = record.retrieved_at_utc;
    provenance.worldview = record.worldview;
    return provenance;
}

[[nodiscard]] std::vector<FeatureKey> feature_keys(
    const std::vector<storage::FeatureGeometryIndexEntry>& features
) {
    std::vector<FeatureKey> keys;
    keys.reserve(features.size());
    for (const auto& feature : features) {
        keys.emplace_back(feature.stable_id, feature.source_feature_id);
    }
    std::sort(keys.begin(), keys.end());
    return keys;
}

[[nodiscard]] std::vector<FeatureKey> feature_keys(
    const std::vector<storage::FeaturePropertiesIndexEntry>& features
) {
    std::vector<FeatureKey> keys;
    keys.reserve(features.size());
    for (const auto& feature : features) {
        keys.emplace_back(feature.stable_id, feature.source_feature_id);
    }
    std::sort(keys.begin(), keys.end());
    return keys;
}

}  // namespace

DurableSourceLoadResult load_durable_source_result(
    const storage::ProjectStore& project,
    const std::string_view source_id
) {
    if (source_id.empty() || source_id.find('\0') != std::string_view::npos) {
        return failure(
            DurableSourceLoadError::invalid_request,
            "durable source rehydration requires a non-empty NUL-free source ID"
        );
    }

    const storage::SourceSnapshotListResult snapshots =
        storage::list_source_snapshots(project);
    if (!snapshots.ok()) {
        return storage_failure(
            snapshots.status,
            "could not read durable source provenance catalog"
        );
    }
    const storage::SourceSnapshotRecord* record = find_source(snapshots.records, source_id);
    if (record == nullptr) {
        return failure(
            DurableSourceLoadError::source_not_found,
            "durable source ID is not present in project provenance"
        );
    }
    if (record->materialization_state != storage::SourceMaterializationState::materialized) {
        return failure(
            DurableSourceLoadError::geometry_unavailable,
            "durable source is Referenced; canonical project geometry is not materialized"
        );
    }

    const storage::SourceGeometryIndexResult geometry_index =
        storage::list_source_geometry_index(project, source_id);
    if (!geometry_index.ok()) {
        if (geometry_index.status.error == storage::StorageError::record_not_found) {
            return failure(
                DurableSourceLoadError::geometry_unavailable,
                "Materialized durable source does not contain canonical feature geometry"
            );
        }
        return storage_failure(
            geometry_index.status,
            "could not enumerate durable source geometry"
        );
    }
    if (geometry_index.features.empty()) {
        return failure(
            DurableSourceLoadError::geometry_unavailable,
            "Materialized durable source contains an empty canonical geometry set"
        );
    }

    bool properties_complete = false;
    storage::SourceFeaturePropertiesIndexResult properties =
        storage::list_source_feature_properties_index(project, source_id);
    if (properties.ok()) {
        properties_complete = true;
        if (feature_keys(geometry_index.features) != feature_keys(properties.features)) {
            return failure(
                DurableSourceLoadError::storage_rejected,
                "complete property index does not describe the canonical geometry feature set",
                storage::StorageError::schema_invalid
            );
        }
    } else if (properties.status.error != storage::StorageError::record_not_found) {
        return storage_failure(
            properties.status,
            "could not inspect durable feature-property completeness"
        );
    }

    DurableSourceLoadResult result{};
    result.source.provenance = rehydrate_provenance(*record);
    if (!result.source.provenance.complete()) {
        return failure(
            DurableSourceLoadError::storage_rejected,
            "durable source provenance is incomplete for source::Result rehydration",
            storage::StorageError::schema_invalid
        );
    }
    result.source.feature_properties_complete = properties_complete;
    result.source.features.reserve(geometry_index.features.size());

    for (const storage::FeatureGeometryIndexEntry& entry : geometry_index.features) {
        const storage::FeatureGeometryLoadResult loaded =
            storage::load_feature_geometry(project, source_id, entry.stable_id);
        if (!loaded.ok()) {
            return storage_failure(
                loaded.status,
                "could not load durable feature geometry for " + entry.stable_id
            );
        }

        source::Feature feature{};
        feature.stable_id = loaded.feature->stable_id;
        feature.source_id = loaded.feature->source_feature_id;
        feature.rings.reserve(loaded.feature->rings.size());

        for (const storage::GeographicRingRecord& stored_ring : loaded.feature->rings) {
            source::FeatureRing ring{};
            if (!map_role(stored_ring.role, ring.role) ||
                !map_interior_side(stored_ring.interior_side, ring.geometry.interior_side)) {
                return failure(
                    DurableSourceLoadError::unsupported_topology,
                    "durable feature contains unsupported ring topology enum"
                );
            }
            const long long winding = static_cast<long long>(stored_ring.longitude_winding);
            if (winding < static_cast<long long>(std::numeric_limits<int>::min()) ||
                winding > static_cast<long long>(std::numeric_limits<int>::max())) {
                return failure(
                    DurableSourceLoadError::unsupported_topology,
                    "durable ring longitude winding exceeds the in-memory int domain"
                );
            }
            ring.geometry.longitude_winding = static_cast<int>(winding);
            ring.geometry.closing_longitude_rad = stored_ring.closing_longitude_rad;
            ring.geometry.vertices.reserve(stored_ring.vertices.size());
            for (const storage::GeographicPointRecord& point : stored_ring.vertices) {
                ring.geometry.vertices.push_back({point.longitude_rad, point.latitude_rad});
            }
            feature.rings.push_back(std::move(ring));
        }

        if (properties_complete) {
            const storage::FeaturePropertiesLoadResult loaded_properties =
                storage::load_feature_properties(project, source_id, entry.stable_id);
            if (!loaded_properties.ok()) {
                return storage_failure(
                    loaded_properties.status,
                    "could not load complete durable feature properties for " + entry.stable_id
                );
            }
            feature.properties.reserve(loaded_properties.properties.size());
            for (const storage::StoredFeatureProperty& property : loaded_properties.properties) {
                feature.properties.push_back({
                    property.key,
                    map_property_value(property.value),
                });
            }
        }

        result.source.features.push_back(std::move(feature));
    }

    result.source.error = source::SourceError::none;
    return result;
}

}  // namespace aeris::project
