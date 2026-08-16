// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "aeris/storage/geography.hpp"
#include "aeris/storage/status.hpp"

#include <string>
#include <vector>

struct sqlite3;

namespace aeris::storage::detail {

Status canonicalize_feature_records(std::vector<SourceFeatureRecord>& features);
[[nodiscard]] bool equal_feature_records(
    const std::vector<SourceFeatureRecord>& a,
    const std::vector<SourceFeatureRecord>& b) noexcept;
Status insert_feature_records(
    sqlite3* db,
    const std::string& source_id,
    const std::vector<SourceFeatureRecord>& features);
Status load_feature_records(
    sqlite3* db,
    const std::string& source_id,
    std::vector<SourceFeatureRecord>& features);

}  // namespace aeris::storage::detail
