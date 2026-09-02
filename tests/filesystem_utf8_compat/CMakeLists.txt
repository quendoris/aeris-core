# SPDX-FileCopyrightText: 2026 quendoris
# SPDX-License-Identifier: AGPL-3.0-only

cmake_minimum_required(VERSION 3.16)
project(aeris_filesystem_utf8_compat LANGUAGES CXX)

add_executable(aeris_filesystem_utf8_compat main.cpp)
target_include_directories(aeris_filesystem_utf8_compat PRIVATE ../../include)
target_compile_features(aeris_filesystem_utf8_compat PRIVATE cxx_std_20)
set_target_properties(aeris_filesystem_utf8_compat PROPERTIES CXX_EXTENSIONS OFF)

if(MSVC)
    target_compile_options(aeris_filesystem_utf8_compat PRIVATE /W4 /WX /permissive-)
else()
    target_compile_options(
        aeris_filesystem_utf8_compat
        PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror
    )
endif()

enable_testing()
add_test(NAME filesystem_utf8_cxx20 COMMAND aeris_filesystem_utf8_compat)
