// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "aeris/storage/geometry.hpp"
#include "aeris/storage/project.hpp"

#include <optional>
#include <string>

#include <sqlite3.h>

namespace aeris::storage::detail {

// Canonicalizes and validates a storage-native source geometry record using the
// exact same codec contract as the public geometry mutation path.
[[nodiscard]] Status canonicalize_and_validate_source_geometry_record(
    SourceGeometryRecord& record);

[[nodiscard]] bool equal_source_geometry_records(
    const SourceGeometryRecord& a,
    const SourceGeometryRecord& b);

// Transaction-level primitives. The caller owns the SQLite transaction and is
// responsible for project identity/durability setup and revision semantics.
[[nodiscard]] Status load_source_geometry_record(
    sqlite3* db,
    const std::string& source_id,
    std::optional<SourceGeometryRecord>& record);

[[nodiscard]] Status insert_source_geometry_record(
    sqlite3* db,
    const SourceGeometryRecord& record);

// Performs the expensive semantic pass over canonical geometry payloads.
// Project open intentionally does not call this; explicit integrity audit does.
[[nodiscard]] Status verify_geometry_semantics(const ProjectStore& project);

}  // namespace aeris::storage::detail
