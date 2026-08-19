// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "aeris/storage/project.hpp"
#include "provenance_detail.hpp"

namespace aeris::storage::detail {

// The geometry translation unit owns the expensive payload implementation under
// an internal symbol name. All other storage callers see the wrapper below, so
// generation-9 source-state semantics are always audited before canonical
// geometry payloads. This keeps ProjectStore::verify_integrity() source-ordering
// explicit without duplicating the geometry scanner or changing public APIs.
#if defined(AERIS_GEOMETRY_VERIFIER_IMPLEMENTATION)
#define verify_geometry_semantics verify_geometry_payload_semantics
[[nodiscard]] Status verify_geometry_semantics(const ProjectStore& project);
#else
[[nodiscard]] Status verify_geometry_payload_semantics(const ProjectStore& project);

[[nodiscard]] inline Status verify_geometry_semantics(const ProjectStore& project) {
    Status status = verify_source_semantics(project);
    if (!status) return status;
    return verify_geometry_payload_semantics(project);
}
#endif

}  // namespace aeris::storage::detail
