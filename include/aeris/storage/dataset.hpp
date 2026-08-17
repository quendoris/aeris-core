// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "aeris/storage/feature_property.hpp"
#include "aeris/storage/geometry.hpp"
#include "aeris/storage/provenance.hpp"

#include <optional>
#include <string_view>

namespace aeris::storage {

// One product-level immutable source dataset. Provenance/resource identity and
// canonical geographic geometry are always recorded together. A complete
// feature-property channel is optional: absence means properties were not
// recorded by this request, while presence is authoritative for every geometry
// feature, including verified-empty per-feature lists.
struct SourceDatasetRecord final {
    SourceSnapshotRecord source;
    SourceGeometryRecord geometry;
    std::optional<SourceFeaturePropertiesRecord> feature_properties;
};

struct SourceDatasetMutationResult final {
    Status status;
    bool inserted{false};
    bool durably_committed{false};

    [[nodiscard]] bool ok() const noexcept { return status.ok(); }
};

// Atomically records source provenance/resources plus the explicit canonical
// geometry marker and, when supplied, the complete feature-property marker and
// all typed property payloads in one SQLite transaction and one project revision.
// Exact retries are idempotent.
//
// Existing identical provenance/geometry without feature properties is a valid
// lower-level state. Supplying complete properties later attaches them in one
// new transaction. Conversely, a later geometry-only retry never deletes or
// demotes an already-recorded complete property channel because omission means
// "no property write requested", not "properties must be absent".
[[nodiscard]] SourceDatasetMutationResult store_source_dataset(
    ProjectStore& project,
    const SourceDatasetRecord& record,
    std::string_view modified_utc);

}  // namespace aeris::storage
