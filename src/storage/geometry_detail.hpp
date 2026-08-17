// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "aeris/storage/geometry.hpp"
#include "mutation_detail.hpp"

struct sqlite3;

namespace aeris::storage::detail {

// Canonicalizes deterministic geometry identity and validates the exact same
// WGS84/topology contract used by the public low-level mutation.
[[nodiscard]] Status prepare_source_geometry(SourceGeometryRecord& record);

// Inspects immutable canonical geometry using an already-open connection. The
// caller owns transaction boundaries; this function never commits or rolls back.
[[nodiscard]] Status inspect_source_geometry(
    sqlite3* db,
    const SourceGeometryRecord& record,
    ExistingRecordState& state);

// Inserts only the source-geometry marker, features, rings and BLOB payloads.
// The caller owns the transaction and project-revision update.
[[nodiscard]] Status insert_source_geometry_rows(
    sqlite3* db,
    const SourceGeometryRecord& record);

// Performs the expensive semantic pass over canonical geometry payloads.
// Project open intentionally does not call this; explicit integrity audit does.
[[nodiscard]] Status verify_geometry_semantics(const ProjectStore& project);

}  // namespace aeris::storage::detail
