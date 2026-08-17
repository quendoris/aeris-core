// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "aeris/storage/project.hpp"

namespace aeris::storage::detail {

// Performs the expensive semantic/content pass over general project resources,
// including embedded chunk hashes, aggregate SHA-256, size accounting and the
// portable/frozen invariant. Project open intentionally does not hash every
// embedded payload; explicit integrity verification does.
[[nodiscard]] Status verify_resource_semantics(const ProjectStore& project);

}  // namespace aeris::storage::detail
