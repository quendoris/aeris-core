// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <filesystem>
#include <string>

namespace aeris::util {

// Return the UTF-8 byte representation of a filesystem path regardless of
// whether std::filesystem::path::u8string() uses char (C++17) or char8_t
// (C++20 and later). Consumers such as SQLite accept UTF-8 as const char*, so
// the boundary deliberately preserves the encoded bytes without a text-codec
// round trip.
[[nodiscard]] inline std::string filesystem_path_utf8(const std::filesystem::path& path) {
    const auto utf8 = path.u8string();
    return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

}  // namespace aeris::util
