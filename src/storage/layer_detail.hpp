// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "aeris/storage/project.hpp"

namespace aeris::storage::detail {

[[nodiscard]] Status verify_layer_semantics(const ProjectStore& project);

}  // namespace aeris::storage::detail
