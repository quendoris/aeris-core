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

// Built-in presentation roles are semantic IDs, not renderer class names. They
// are suitable for ProjectLayerRecord::role_id and remain independent of Qt or
// any particular frontend. Pre-1.0 additions may still evolve, but one role ID
// always denotes one meaning within a given draft generation.
inline constexpr std::string_view kLayerRolePhysicalLandFillV1 =
    "aeris.layer.physical.land-fill.v1";
inline constexpr std::string_view kLayerRolePhysicalCoastlineV1 =
    "aeris.layer.physical.coastline.v1";
inline constexpr std::string_view kLayerRolePoliticalCountryFillV1 =
    "aeris.layer.political.country-fill.v1";
inline constexpr std::string_view kLayerRolePoliticalBoundaryV1 =
    "aeris.layer.political.boundary.v1";
inline constexpr std::string_view kLayerRoleCountryLabelV1 =
    "aeris.layer.label.country.v1";

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

// Initializes an empty project with one complete ordered layer stack in a single
// acknowledged transaction and one project revision. Input order becomes the
// canonical ordinal order 0..N-1. Source/resource slot validation, resource
// promotion and frozen-project invalidation happen inside the same transaction.
//
// Exact retry is idempotent: if the complete existing stack is byte-for-byte
// equivalent to the supplied canonicalized requests, success is returned with
// changed=false and no revision advance. Any different non-empty existing stack
// is rejected rather than partially merged with the requested initialization.
[[nodiscard]] LayerMutationResult initialize_layer_stack(
    ProjectStore& project,
    const std::vector<LayerCreateRequest>& layers,
    std::string_view modified_utc);

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
