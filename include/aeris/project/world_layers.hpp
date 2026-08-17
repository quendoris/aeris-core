// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "aeris/storage/layer.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace aeris::project {

inline constexpr std::string_view kBuiltinPhysicalLandLayerId =
    "builtin.physical.land";
inline constexpr std::string_view kBuiltinPhysicalCoastlineLayerId =
    "builtin.physical.coastline";
inline constexpr std::string_view kBuiltinPoliticalCountriesLayerId =
    "builtin.political.countries";
inline constexpr std::string_view kBuiltinPoliticalBordersLayerId =
    "builtin.political.borders";
inline constexpr std::string_view kBuiltinPoliticalLabelsLayerId =
    "builtin.political.labels";

struct BuiltinWorldLayerSources final {
    std::string physical_source_id;
    std::string political_source_id;
};

enum class WorldLayerStackError : std::uint8_t {
    none = 0U,
    invalid_request,
    source_contract_mismatch,
    storage_rejected,
};

struct WorldLayerStackResult final {
    WorldLayerStackError error = WorldLayerStackError::none;
    storage::StorageError storage_error = storage::StorageError::none;
    bool changed = false;
    bool durably_committed = false;
    std::string diagnostic;

    [[nodiscard]] bool ok() const noexcept {
        return error == WorldLayerStackError::none;
    }
};

// Compose the built-in AERIS Physical + Political workbench layers over source
// datasets that are already durably present in the project. This function owns
// semantic slot wiring so frontends never need to know that Country labels bind
// to the political `properties` channel while Borders bind to `geometry`.
//
// Preconditions are intentionally stronger than mere source-row existence:
// - physical source advertises the land capability and has stored geometry;
// - political source advertises admin0, has explicit worldview provenance,
//   stored geometry, and a complete per-feature property index matching that
//   geometry cardinality.
//
// The complete five-layer stack is delegated to storage::initialize_layer_stack
// and therefore becomes one acknowledged project revision, with exact-retry and
// concurrency semantics inherited from that storage contract.
[[nodiscard]] WorldLayerStackResult initialize_builtin_world_layer_stack(
    storage::ProjectStore& project,
    const BuiltinWorldLayerSources& sources,
    std::string_view modified_utc);

}  // namespace aeris::project
