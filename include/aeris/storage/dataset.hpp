// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "aeris/storage/geometry.hpp"
#include "aeris/storage/provenance.hpp"

#include <string_view>

namespace aeris::storage {

// One product-level immutable source dataset. The provenance/resource identity
// and canonical geographic geometry are one acknowledged project mutation.
// The two source IDs are deliberately explicit and must agree; keeping the
// existing lower-level record types avoids creating a second geometry model.
struct SourceDatasetRecord final {
    SourceSnapshotRecord source;
    SourceGeometryRecord geometry;
};

struct SourceDatasetMutationResult final {
    Status status;
    bool inserted{false};
    bool durably_committed{false};

    [[nodiscard]] bool ok() const noexcept { return status.ok(); }
};

// Atomically records source provenance/resources plus the explicit canonical
// geometry marker and all feature/ring payloads in one SQLite transaction and
// advances the project revision exactly once. Exact retries are idempotent.
//
// Existing identical provenance without a geometry marker is a valid lower-
// level state (store_source_snapshot may be used for non-vector sources); in
// that case this call may attach the requested canonical geometry in one new
// transaction. A geometry marker without its provenance parent is corruption.
[[nodiscard]] SourceDatasetMutationResult store_source_dataset(
    ProjectStore& project,
    const SourceDatasetRecord& record,
    std::string_view modified_utc);

}  // namespace aeris::storage
