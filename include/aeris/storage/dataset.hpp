// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "aeris/storage/geometry.hpp"
#include "aeris/storage/provenance.hpp"

#include <string_view>

namespace aeris::storage {

// One immutable logical source as acknowledged by the product: exact verified
// provenance/resource identity plus its complete canonical geometry set.
struct SourceDatasetRecord final {
    SourceSnapshotRecord provenance;
    SourceGeometryRecord geometry;
};

struct SourceDatasetMutationResult final {
    Status status;
    bool inserted{false};
    bool durably_committed{false};

    [[nodiscard]] bool ok() const noexcept { return status.ok(); }
};

// Atomically records provenance and canonical geometry in one SQLite
// transaction and advances the project revision exactly once. Existing partial
// low-level state is rejected rather than silently being reclassified as an
// atomic product ingestion.
[[nodiscard]] SourceDatasetMutationResult store_source_dataset(
    ProjectStore& project,
    const SourceDatasetRecord& record,
    std::string_view modified_utc);

}  // namespace aeris::storage
