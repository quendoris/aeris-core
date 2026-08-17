// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "aeris/storage/project.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace aeris::storage {

inline constexpr std::size_t kMaxFeaturePropertiesPerFeature = 4096U;
inline constexpr std::size_t kMaxFeaturePropertyTextBytes = 1024U * 1024U;

inline constexpr std::string_view kFeaturePropertiesModelId =
    "aeris.feature-properties.v1";
inline constexpr std::string_view kFeaturePropertiesEncodingId =
    "aeris.feature-properties.typed-canonical.v1";
inline constexpr std::string_view kFeaturePropertyBoolTypeId =
    "aeris.feature.value.bool.v1";
inline constexpr std::string_view kFeaturePropertyInt64LeTypeId =
    "aeris.feature.value.i64le.v1";
inline constexpr std::string_view kFeaturePropertyF64LeTypeId =
    "aeris.feature.value.f64le.v1";
inline constexpr std::string_view kFeaturePropertyUtf8TypeId =
    "aeris.feature.value.utf8.v1";

using StoredFeaturePropertyValue =
    std::variant<bool, std::int64_t, double, std::string>;

struct StoredFeatureProperty final {
    std::string key;
    StoredFeaturePropertyValue value;
};

// The complete input representation includes one entry for every canonical
// geometry feature, even when that feature has zero properties. Storage may
// omit empty per-feature rows because the source-level completeness marker plus
// exact feature_count preserves the verified-empty distinction.
struct FeaturePropertiesRecord final {
    std::string stable_id;
    std::vector<StoredFeatureProperty> properties;
};

struct SourceFeaturePropertiesRecord final {
    std::string source_id;
    std::vector<FeaturePropertiesRecord> features;
};

struct FeaturePropertiesMutationResult final {
    Status status;
    bool inserted{false};
    bool durably_committed{false};

    [[nodiscard]] bool ok() const noexcept { return status.ok(); }
};

struct FeaturePropertiesIndexEntry final {
    std::string stable_id;
    std::string source_feature_id;
    std::uint32_t property_count{0U};
};

struct SourceFeaturePropertiesIndexResult final {
    Status status;
    std::vector<FeaturePropertiesIndexEntry> features;

    [[nodiscard]] bool ok() const noexcept { return status.ok(); }
};

struct FeaturePropertiesLoadResult final {
    Status status;
    std::vector<StoredFeatureProperty> properties;

    [[nodiscard]] bool ok() const noexcept { return status.ok(); }
};

// Attaches one complete immutable feature-property set to an already persisted
// canonical source geometry. The supplied feature set must be an exact stable-ID
// match for that geometry; exact retries are idempotent.
[[nodiscard]] FeaturePropertiesMutationResult store_source_feature_properties(
    ProjectStore& project,
    const SourceFeaturePropertiesRecord& record,
    std::string_view modified_utc);

// A missing completeness marker returns record_not_found. A present marker
// enumerates every geometry feature in stable-ID order, including zero-property
// features with property_count=0.
[[nodiscard]] SourceFeaturePropertiesIndexResult list_source_feature_properties_index(
    const ProjectStore& project,
    std::string_view source_id);

// Loads one feature's properties in canonical key order. A verified-empty
// feature returns success with an empty vector; a missing source marker or
// feature returns record_not_found.
[[nodiscard]] FeaturePropertiesLoadResult load_feature_properties(
    const ProjectStore& project,
    std::string_view source_id,
    std::string_view stable_id);

}  // namespace aeris::storage
