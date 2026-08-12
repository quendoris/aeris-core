# SPDX-FileCopyrightText: 2026 quendoris
# SPDX-License-Identifier: AGPL-3.0-only

cmake_minimum_required(VERSION 3.16)

project(
    aeris
    VERSION 0.1.0
    DESCRIPTION "Authalic Equal-area Rendering & Ingestion System"
    LANGUAGES CXX
)

option(AERIS_BUILD_TESTS "Build AERIS tests" ON)

add_library(aeris_core STATIC
    src/geo/wgs84.cpp
)

add_library(AERIS::core ALIAS aeris_core)

target_include_directories(aeris_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(aeris_core PUBLIC cxx_std_17)
set_target_properties(aeris_core PROPERTIES
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)

if(MSVC)
    target_compile_options(aeris_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(aeris_core PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
    )
endif()

if(AERIS_BUILD_TESTS)
    include(CTest)
    enable_testing()
    add_subdirectory(tests)
endif()
