// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "aeris/storage/project.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aeris::storage {

inline constexpr std::size_t kMaxProjectLayers = 65535U;
inline constexpr std::size_t kMaxLayerBindings = 256U;

struct LayerSourceBinding final {
    std::string slot_id;
    std::string source_id;
};

struct LayerResourceBinding final {
    std::string slot_id;
    std::string resource_id;
};

struct ProjectLayerRecord final {
    std::string layer_id;
    std::string role_id;
    std::string name;
    std::uint32_t ordinal{0U};
    bool visible{true};
    std::vector<LayerSourceBinding> sources;
    std::vector<LayerResourceBinding> resources;
};

struct LayerCreateRequest final {
    std::string layer_id;
    std::string role_id;
    std::string name;
    bool visible{true};
    std::vector<LayerSourceBinding> sources;
    std::vector<LayerResourceBinding> resources;
};

struct LayerMutationResult final {
    Status status;
    bool changed{false};
    bool durably_committed{false};

    [[nodiscard]] bool ok() const noexcept { return status.ok(); }
};

struct ProjectLayerListResult final {
    Status status;
    std::vector<ProjectLayerRecord> records;

    [[nodiscard]] bool ok() const noexcept { return status.ok(); }
};

struct LayerStateUpdate final {
    std::string modified_utc;
    std::optional<std::string> name;
    std::optional<bool> visible;
};

// Appends one complete logical layer at the top of the current stack. Source and
// resource slots are part of the same acknowledged transaction. Resource slots
// automatically promote their referenced resource to required-for-reproduction;
// binding an external resource therefore also invalidates a previously frozen
// project in that same transaction.
[[nodiscard]] LayerMutationResult append_layer(
    ProjectStore& project,
    const LayerCreateRequest& request,
    std::string_view modified_utc);

[[nodiscard]] LayerMutationResult update_layer_state(
    ProjectStore& project,
    std::string_view layer_id,
    const LayerStateUpdate& update);

// The supplied IDs must be an exact permutation of the current layer set. One
// transaction rewrites contiguous ordinals 0..N-1 and advances one revision.
[[nodiscard]] LayerMutationResult set_layer_order(
    ProjectStore& project,
    const std::vector<std::string>& ordered_layer_ids,
    std::string_view modified_utc);

// Removes the layer and its source/resource slot bindings, then compacts the
// remaining stack. Referenced resources themselves remain project resources;
// removal never silently deletes content-addressed bytes.
[[nodiscard]] LayerMutationResult remove_layer(
    ProjectStore& project,
    std::string_view layer_id,
    std::string_view modified_utc);

[[nodiscard]] ProjectLayerListResult list_project_layers(
    const ProjectStore& project);

}  // namespace aeris::storage
