// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "aeris/storage/project.hpp"

namespace aeris::storage::detail {

// Performs the expensive semantic pass over canonical geometry payloads.
// Project open intentionally does not call this; explicit integrity audit does.
[[nodiscard]] Status verify_geometry_semantics(const ProjectStore& project);

}  // namespace aeris::storage::detail
