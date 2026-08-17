// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "aeris/storage/feature_property.hpp"

#include <optional>

struct sqlite3;

namespace aeris::storage::detail {

[[nodiscard]] Status canonicalize_and_validate_feature_properties(
    SourceFeaturePropertiesRecord& record);

[[nodiscard]] bool equal_feature_properties(
    const SourceFeaturePropertiesRecord& a,
    const SourceFeaturePropertiesRecord& b) noexcept;

[[nodiscard]] Status insert_feature_properties(
    sqlite3* db,
    const SourceFeaturePropertiesRecord& record);

[[nodiscard]] Status read_existing_feature_properties(
    const ProjectStore& project,
    std::string_view source_id,
    std::optional<SourceFeaturePropertiesRecord>& record);

[[nodiscard]] Status verify_feature_property_semantics(
    const ProjectStore& project);

}  // namespace aeris::storage::detail
