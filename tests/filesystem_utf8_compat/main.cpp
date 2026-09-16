// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/util/filesystem_utf8.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <type_traits>

int main() {
    using U8String = decltype(std::filesystem::path{}.u8string());
    static_assert(
        std::is_same_v<typename U8String::value_type, char8_t>,
        "C++20 compatibility probe must exercise the char8_t u8string boundary"
    );

    const std::filesystem::path path{
        u8"aeris-\u0442\u0435\u0441\u0442/\u043a\u0430\u0440\u0442\u0430.aeris"
    };
    const std::string expected =
        "aeris-\xD1\x82\xD0\xB5\xD1\x81\xD1\x82/"
        "\xD0\xBA\xD0\xB0\xD1\x80\xD1\x82\xD0\xB0.aeris";
    const std::string actual = aeris::util::filesystem_path_utf8(path);

    if (actual != expected) {
        std::cerr << "filesystem UTF-8 boundary changed encoded bytes\n";
        return 1;
    }
    return 0;
}
