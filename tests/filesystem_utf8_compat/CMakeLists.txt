# SPDX-FileCopyrightText: 2026 quendoris
# SPDX-License-Identifier: AGPL-3.0-only

cmake_minimum_required(VERSION 3.16)
project(aeris_filesystem_utf8_compat LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(AERIS_BUILD_STORAGE ON CACHE BOOL "" FORCE)
set(AERIS_BUILD_PROJECT OFF CACHE BOOL "" FORCE)
set(AERIS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(AERIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(AERIS_BUILD_VIEWER OFF CACHE BOOL "" FORCE)
set(AERIS_WARNINGS_AS_ERRORS ON CACHE BOOL "" FORCE)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../.." "${CMAKE_BINARY_DIR}/aeris-core")

add_executable(aeris_filesystem_utf8_compat main.cpp)
target_include_directories(aeris_filesystem_utf8_compat PRIVATE ../../include)
target_compile_features(aeris_filesystem_utf8_compat PRIVATE cxx_std_20)
set_target_properties(aeris_filesystem_utf8_compat PROPERTIES CXX_EXTENSIONS OFF)

add_executable(aeris_storage_unicode_path ../test_storage_unicode_path.cpp)
target_link_libraries(aeris_storage_unicode_path PRIVATE AERIS::storage)
target_compile_features(aeris_storage_unicode_path PRIVATE cxx_std_20)
set_target_properties(aeris_storage_unicode_path PROPERTIES CXX_EXTENSIONS OFF)

if(MSVC)
    target_compile_options(aeris_filesystem_utf8_compat PRIVATE /W4 /WX /permissive-)
    target_compile_options(aeris_storage_unicode_path PRIVATE /W4 /WX /permissive-)
else()
    target_compile_options(
        aeris_filesystem_utf8_compat
        PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror
    )
    target_compile_options(
        aeris_storage_unicode_path
        PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror
    )
endif()

enable_testing()
add_test(NAME filesystem_utf8_cxx20 COMMAND aeris_filesystem_utf8_compat)
add_test(NAME storage_unicode_path_cxx20 COMMAND aeris_storage_unicode_path)
