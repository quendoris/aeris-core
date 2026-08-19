// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "aeris/storage/project.hpp"
#include "provenance_detail.hpp"

namespace aeris::storage::detail {

// The geometry translation unit owns its legacy first-generation mutation and
// expensive payload verifier under internal symbol names. Generation 9 exposes
// compatibility facades elsewhere: geometry mutation delegates to the atomic
// dataset transition, while deep audit always checks source state first.
#if defined(AERIS_GEOMETRY_VERIFIER_IMPLEMENTATION)
#define verify_geometry_semantics verify_geometry_payload_semantics
#define store_source_geometry store_source_geometry_legacy_impl
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
