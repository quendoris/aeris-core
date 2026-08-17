// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#include "aeris/project/dataset_ingest.hpp"

#include "aeris/storage/dataset.hpp"
#include "source_bridge_detail.hpp"

#include <cstdint>
#include <limits>
#include <utility>

namespace aeris::project {
namespace {

[[nodiscard]] bool map_ring_role(
    const source::RingRole source_role,
    storage::StoredRingRole& stored_role) noexcept {
    switch (source_role) {
        case source::RingRole::exterior:
            stored_role = storage::StoredRingRole::exterior;
            return true;
        case source::RingRole::interior:
            stored_role = storage::StoredRingRole::interior;
            return true;
    }
    return false;
}

[[nodiscard]] bool map_interior_side(
    const geometry::RingInteriorSide source_side,
    storage::StoredInteriorSide& stored_side) noexcept {
    switch (source_side) {
        case geometry::RingInteriorSide::unspecified:
            stored_side = storage::StoredInteriorSide::unspecified;
            return true;
        case geometry::RingInteriorSide::left:
            stored_side = storage::StoredInteriorSide::left;
            return true;
        case geometry::RingInteriorSide::right:
            stored_side = storage::StoredInteriorSide::right;
            return true;
    }
    return false;
}

[[nodiscard]] SourceBridgeResult map_geometry(
    const source::Result& source_result,
    const std::string& project_source_id,
    storage::SourceGeometryRecord& geometry_record) {
    geometry_record.source_id = project_source_id;
    geometry_record.features.reserve(source_result.features.size());

    for (const source::Feature& source_feature : source_result.features) {
        storage::FeatureGeometryRecord feature{};
        feature.stable_id = source_feature.stable_id;
        feature.source_feature_id = source_feature.source_id;
        feature.rings.reserve(source_feature.rings.size());

        for (const source::FeatureRing& source_ring : source_feature.rings) {
            storage::GeographicRingRecord ring{};
            if (!map_ring_role(source_ring.role, ring.role) ||
                !map_interior_side(source_ring.ring.interior_side, ring.interior_side)) {
                return detail::bridge_failure(
                    SourceBridgeError::geometry_mapping_failed,
                    "adapter result contains a ring enum value outside the canonical project mapping domain");
            }
            if (source_ring.ring.longitude_winding <
                    static_cast<int>(std::numeric_limits<std::int32_t>::min()) ||
                source_ring.ring.longitude_winding >
                    static_cast<int>(std::numeric_limits<std::int32_t>::max())) {
                return detail::bridge_failure(
                    SourceBridgeError::geometry_mapping_failed,
                    "adapter ring longitude winding exceeds the canonical project int32 range");
            }

            ring.longitude_winding =
                static_cast<std::int32_t>(source_ring.ring.longitude_winding);
            ring.closing_longitude_rad = source_ring.ring.closing_longitude_rad;
            ring.vertices.reserve(source_ring.ring.vertices.size());
            for (const geometry::GeodeticPoint point : source_ring.ring.vertices) {
                ring.vertices.push_back({point.longitude_rad, point.latitude_rad});
            }
            feature.rings.push_back(std::move(ring));
        }
        geometry_record.features.push_back(std::move(feature));
    }

    return {};
}

}  // namespace

SourceBridgeResult ingest_verified_source_dataset(
    storage::ProjectStore& project,
    const source::AdapterRegistry& registry,
    const source::VerifiedSnapshot& snapshot,
    const VerifiedSourceRecordRequest& request) {
    auto prepared = detail::prepare_verified_source(registry, snapshot, request);
    if (!prepared.ok()) return std::move(prepared.status);

    storage::SourceDatasetRecord dataset{};
    dataset.provenance = std::move(prepared.value->provenance);

    SourceBridgeResult mapped = map_geometry(
        prepared.value->source_result,
        request.source_id,
        dataset.geometry);
    if (!mapped.ok()) return mapped;

    const storage::SourceDatasetMutationResult stored =
        storage::store_source_dataset(project, dataset, request.modified_utc);
    if (!stored.ok()) {
        SourceBridgeResult result = detail::bridge_failure(
            SourceBridgeError::storage_rejected,
            stored.status.diagnostic.empty()
                ? "project storage rejected verified source dataset"
                : stored.status.diagnostic);
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
