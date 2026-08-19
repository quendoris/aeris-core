// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/storage/dataset.hpp"
#include "aeris/storage/geometry.hpp"
#include "aeris/storage/provenance.hpp"

#include <string>
#include <utility>

namespace aeris::storage {

SourceGeometryMutationResult store_source_geometry(
    ProjectStore& project,
    const SourceGeometryRecord& geometry,
    const std::string_view modified_utc) {
    if (!is_canonical_utc_timestamp(modified_utc)) {
        return {{StorageError::invalid_argument,
                 "geometry mutation timestamp is not canonical Gregorian UTC"},
                false, false};
    }

    const SourceSnapshotListResult sources = list_source_snapshots(project);
    if (!sources.ok()) return {sources.status, false, false};

    const SourceSnapshotRecord* source = nullptr;
    for (const SourceSnapshotRecord& candidate : sources.records) {
        if (candidate.source_id == geometry.source_id) {
            source = &candidate;
            break;
        }
    }
    if (source == nullptr) {
        return {{StorageError::record_not_found,
                 "canonical geometry requires an already-persisted source provenance row"},
                false, false};
    }

    SourceDatasetRecord dataset{};
    dataset.source = *source;
    dataset.geometry = geometry;

    // This compatibility API predates explicit source materialization. A first
    // geometry write now means a geometry-only Materialize operation and must
    // therefore cross the same atomic dataset transaction as the product path.
    // If the reference did not already record a verified retrieval time, the
    // acknowledged materialization time becomes that lower-level timestamp.
    if (dataset.source.materialization_state == SourceMaterializationState::referenced &&
        dataset.source.retrieved_at_utc.empty()) {
        dataset.source.retrieved_at_utc = std::string(modified_utc);
    }
    dataset.source.materialization_state = SourceMaterializationState::materialized;

    SourceDatasetMutationResult result =
        store_source_dataset(project, dataset, modified_utc);
    return {std::move(result.status), result.inserted, result.durably_committed};
}

}  // namespace aeris::storage
