// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "aeris/storage/provenance.hpp"

#include <optional>
#include <string>

#include <sqlite3.h>

namespace aeris::storage::detail {

// Canonicalizes and validates the neutral provenance DTO using the exact same
// contract as the public provenance mutation path.
[[nodiscard]] Status canonicalize_and_validate_source_snapshot_record(
    SourceSnapshotRecord& record);

[[nodiscard]] bool equal_source_snapshot_records(
    const SourceSnapshotRecord& a,
    const SourceSnapshotRecord& b);

// Transaction-level primitives. The caller owns the SQLite transaction and is
// responsible for project identity/durability setup and revision semantics.
[[nodiscard]] Status load_source_snapshot_record(
    sqlite3* db,
    const std::string& source_id,
    std::optional<SourceSnapshotRecord>& record);

[[nodiscard]] Status insert_source_snapshot_record(
    sqlite3* db,
    const SourceSnapshotRecord& record);

}  // namespace aeris::storage::detail
