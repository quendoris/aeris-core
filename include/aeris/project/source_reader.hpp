// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "aeris/source/adapter.hpp"
#include "aeris/storage/project.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace aeris::project {

enum class DurableSourceLoadError : std::uint8_t {
    none = 0U,
    invalid_request,
    source_not_found,
    geometry_unavailable,
    unsupported_topology,
    storage_rejected,
};

struct DurableSourceLoadResult final {
    source::Result source;
    DurableSourceLoadError error = DurableSourceLoadError::none;
    storage::StorageError storage_error = storage::StorageError::none;
    std::string diagnostic;

    [[nodiscard]] bool ok() const noexcept {
        return error == DurableSourceLoadError::none && source.ok();
    }
};

// Rehydrate one canonical source::Result exclusively from durable .aeris state.
// No source adapter, acquisition snapshot, SHP/DBF file, or network access is
// consulted on this read path.
//
// Geometry is required because a source::Result without canonical feature
// geometry cannot drive the cartographic core. A missing feature-properties
// completeness marker is not an error: it is faithfully represented as
// feature_properties_complete=false and every Feature::properties remains empty.
// When the marker exists, every canonical feature is loaded with its authoritative
// typed property list, including verified-empty property lists.
[[nodiscard]] DurableSourceLoadResult load_durable_source_result(
    const storage::ProjectStore& project,
    std::string_view source_id);

}  // namespace aeris::project
