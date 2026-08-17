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

inline constexpr std::size_t kMaxProjectStyles = 65535U;
inline constexpr std::size_t kMaxStyleProperties = 1024U;
inline constexpr std::size_t kMaxStyleResourceBindings = 256U;
inline constexpr std::size_t kMaxLayerStyleBindings = 256U;
inline constexpr std::size_t kMaxStylePropertyBytes = 1048576U;

inline constexpr std::string_view kStyleValueBoolV1 = "aeris.style.value.bool.v1";
inline constexpr std::string_view kStyleValueI64LeV1 = "aeris.style.value.i64le.v1";
inline constexpr std::string_view kStyleValueF64LeV1 = "aeris.style.value.f64le.v1";
inline constexpr std::string_view kStyleValueUtf8V1 = "aeris.style.value.utf8.v1";
inline constexpr std::string_view kStyleValueRgba8V1 = "aeris.style.value.rgba8.v1";

// Style values are canonical bytes identified by an explicit versioned type.
// AERIS validates its built-in value types strictly. Other bounded type IDs are
// preserved as opaque canonical payloads so future modules can extend style
// semantics without adding one SQLite column per rendering concept.
struct StylePropertyRecord final {
    std::string property_key;
    std::string value_type_id;
    std::vector<std::uint8_t> value;
};

struct StyleResourceBinding final {
    std::string slot_id;
    std::string resource_id;
};

struct ProjectStyleRecord final {
    std::string style_id;
    std::string model_id;
    std::string name;
    std::optional<std::string> parent_style_id;
    std::vector<StylePropertyRecord> properties;
    std::vector<StyleResourceBinding> resources;
};

struct LayerStyleBinding final {
    std::string slot_id;
    std::string style_id;
};

struct StyleMutationResult final {
    Status status;
    bool changed{false};
    bool durably_committed{false};

    [[nodiscard]] bool ok() const noexcept { return status.ok(); }
};

struct ProjectStyleListResult final {
    Status status;
    std::vector<ProjectStyleRecord> records;

    [[nodiscard]] bool ok() const noexcept { return status.ok(); }
};

struct LayerStyleBindingListResult final {
    Status status;
    std::vector<LayerStyleBinding> records;

    [[nodiscard]] bool ok() const noexcept { return status.ok(); }
};

// Creates or atomically replaces one complete declarative style. Properties and
// resource slots are the style state: no hidden renderer defaults are persisted.
// Exact retry is idempotent. Parent styles must already exist and cycles fail
// closed. Referenced resources are promoted to required-for-reproduction in the
// same transaction; an external style resource therefore thaws a frozen project.
[[nodiscard]] StyleMutationResult set_style(
    ProjectStore& project,
    const ProjectStyleRecord& record,
    std::string_view modified_utc);

// Removal is explicit and refuses styles still referenced by another style or
// by a layer style slot. Content-addressed resources are never deleted/demoted.
[[nodiscard]] StyleMutationResult remove_style(
    ProjectStore& project,
    std::string_view style_id,
    std::string_view modified_utc);

// Replaces the complete named style-slot set for one existing layer in a single
// transaction. Empty bindings are valid and mean that the layer has no attached
// declarative style. Every referenced style must already exist.
[[nodiscard]] StyleMutationResult set_layer_style_bindings(
    ProjectStore& project,
    std::string_view layer_id,
    const std::vector<LayerStyleBinding>& bindings,
    std::string_view modified_utc);

[[nodiscard]] ProjectStyleListResult list_project_styles(
    const ProjectStore& project);

[[nodiscard]] LayerStyleBindingListResult list_layer_style_bindings(
    const ProjectStore& project,
    std::string_view layer_id);

}  // namespace aeris::storage
