// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <cstdint>

namespace aeris::storage::detail {

enum class ExistingRecordState : std::uint8_t {
    absent = 0U,
    identical,
    conflict,
};

}  // namespace aeris::storage::detail
