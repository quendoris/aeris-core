// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/project/source_bridge.hpp"

#include "aeris/storage/provenance.hpp"
#include "source_bridge_detail.hpp"

#include <utility>

namespace aeris::project {

SourceBridgeResult record_verified_source_snapshot(
    storage::ProjectStore& project,
    const source::AdapterRegistry& registry,
    const source::VerifiedSnapshot& snapshot,
    const VerifiedSourceRecordRequest& request
) {
    auto prepared = detail::prepare_verified_source(registry, snapshot, request);
    if (!prepared.ok()) return std::move(prepared.status);

    const storage::SourceSnapshotMutationResult stored =
        storage::store_source_snapshot(project, prepared.value->provenance, request.modified_utc);
    if (!stored.ok()) {
        SourceBridgeResult result = detail::bridge_failure(
            SourceBridgeError::storage_rejected,
            stored.status.diagnostic.empty()
                ? "project storage rejected verified source provenance"
                : stored.status.diagnostic);
        result.storage_error = stored.status.error;
        // A storage error after SQLite commit (for example, failure to refresh
        // the caller's open ProjectStore metadata) must not erase the durable
        // outcome. Recovery logic needs to know that the mutation is already
        // present on disk even though the overall operation returned an error.
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
