// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace aeris::util {

// Locale-free strict UTF-8 validation for durable identifiers/text payloads.
// Rejects embedded NUL, overlong encodings, UTF-16 surrogate code points, and
// scalar values above U+10FFFF. Empty text is valid; callers own emptiness rules.
[[nodiscard]] inline bool is_valid_utf8_nul_free(const std::string_view value) noexcept {
    std::size_t index = 0U;
    while (index < value.size()) {
        const auto lead = static_cast<std::uint8_t>(value[index]);
        if (lead == 0U) return false;
        if (lead <= 0x7fU) {
            ++index;
            continue;
        }

        std::size_t continuation_count = 0U;
        std::uint32_t code_point = 0U;
        std::uint32_t minimum = 0U;
        if (lead >= 0xc2U && lead <= 0xdfU) {
            continuation_count = 1U;
            code_point = static_cast<std::uint32_t>(lead & 0x1fU);
            minimum = 0x80U;
        } else if (lead >= 0xe0U && lead <= 0xefU) {
            continuation_count = 2U;
            code_point = static_cast<std::uint32_t>(lead & 0x0fU);
            minimum = 0x800U;
        } else if (lead >= 0xf0U && lead <= 0xf4U) {
            continuation_count = 3U;
            code_point = static_cast<std::uint32_t>(lead & 0x07U);
            minimum = 0x10000U;
        } else {
            return false;
        }

        if (continuation_count > value.size() - index - 1U) return false;
        for (std::size_t offset = 1U; offset <= continuation_count; ++offset) {
            const auto byte = static_cast<std::uint8_t>(value[index + offset]);
            if ((byte & 0xc0U) != 0x80U) return false;
            code_point = (code_point << 6U) | static_cast<std::uint32_t>(byte & 0x3fU);
        }

        if (code_point < minimum || code_point > 0x10ffffU ||
            (code_point >= 0xd800U && code_point <= 0xdfffU)) {
            return false;
        }
        index += continuation_count + 1U;
    }
    return true;
}

}  // namespace aeris::util
