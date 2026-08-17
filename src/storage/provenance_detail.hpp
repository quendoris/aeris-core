// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "aeris/storage/provenance.hpp"
#include "mutation_detail.hpp"

struct sqlite3;

namespace aeris::storage::detail {

// Canonicalizes caller-order-insensitive fields and validates the exact same
// provenance contract used by the public low-level mutation.
[[nodiscard]] Status prepare_source_snapshot(SourceSnapshotRecord& record);

// Inspects immutable provenance using an already-open connection. The caller
// owns transaction boundaries; this function never commits or rolls back.
[[nodiscard]] Status inspect_source_snapshot(
    sqlite3* db,
    const SourceSnapshotRecord& record,
    ExistingRecordState& state);

// Inserts only aeris_source + aeris_source_resource rows. The caller owns the
// transaction and project-revision update.
[[nodiscard]] Status insert_source_snapshot_rows(
    sqlite3* db,
    const SourceSnapshotRecord& record);

}  // namespace aeris::storage::detail
