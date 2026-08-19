// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "aeris/storage/project.hpp"

namespace aeris::storage::detail {

// Deep source-state audit for generation 9. Referenced sources must not own
// canonical decoded channels; materialized sources must own their canonical
// geometry marker in the current vector-source generation.
[[nodiscard]] Status verify_source_semantics(const ProjectStore& project);

}  // namespace aeris::storage::detail
