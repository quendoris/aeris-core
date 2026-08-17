// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "aeris/project/source_bridge.hpp"

namespace aeris::project {

// Full product ingestion path: registry validation, exact verified acquisition
// cross-check, canonical source-result mapping, then one atomic storage
// transaction for provenance + resources + geometry + one project revision.
[[nodiscard]] SourceBridgeResult ingest_verified_source_dataset(
    storage::ProjectStore& project,
    const source::AdapterRegistry& registry,
    const source::VerifiedSnapshot& snapshot,
    const VerifiedSourceRecordRequest& request);

}  // namespace aeris::project
