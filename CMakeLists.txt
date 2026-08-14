# SPDX-FileCopyrightText: 2026 quendoris
# SPDX-License-Identifier: AGPL-3.0-only

cmake_minimum_required(VERSION 3.16)
project(aeris VERSION 0.1.0 DESCRIPTION "Authalic Equal-area Rendering & Ingestion System" LANGUAGES CXX)

option(AERIS_BUILD_TESTS "Build AERIS tests" ON)
option(AERIS_BUILD_TOOLS "Build AERIS development tools" ON)
option(AERIS_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

function(aeris_enable_warnings target_name)
    if(MSVC)
        target_compile_options(${target_name} PRIVATE /W4 /permissive-)
        if(AERIS_WARNINGS_AS_ERRORS)
            target_compile_options(${target_name} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target_name} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
        if(AERIS_WARNINGS_AS_ERRORS)
            target_compile_options(${target_name} PRIVATE -Werror)
        endif()
    endif()
endfunction()

add_library(aeris_core STATIC
    src/geo/rotation.cpp
    src/geo/wgs84.cpp
    src/geometry/geographic.cpp
    src/geometry/planar.cpp
    src/projection/primitives.cpp
    src/projection/ring.cpp
    src/projection/ring_piecewise.cpp
    src/projection/seam.cpp
    src/projection/subdivide.cpp
    src/projection/wgs84.cpp
    src/source/acquisition.cpp
    src/source/adapter.cpp
    src/source/natural_earth.cpp
    src/source/registry.cpp
    src/source/shapefile.cpp
    src/util/sha256.cpp
    src/view/globe.cpp
    src/view/globe_curve.cpp
)
add_library(AERIS::core ALIAS aeris_core)
target_include_directories(aeris_core PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
target_compile_features(aeris_core PUBLIC cxx_std_17)
set_target_properties(aeris_core PROPERTIES CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
aeris_enable_warnings(aeris_core)

if(AERIS_BUILD_TOOLS)
    add_executable(aeris_diagnostic_svg tools/diagnostic_svg.cpp)
    target_link_libraries(aeris_diagnostic_svg PRIVATE AERIS::core)
    target_compile_features(aeris_diagnostic_svg PRIVATE cxx_std_17)
    set_target_properties(aeris_diagnostic_svg PROPERTIES CXX_EXTENSIONS OFF)
    aeris_enable_warnings(aeris_diagnostic_svg)

    add_executable(aeris_real_world_probe tools/real_world_probe.cpp)
    target_link_libraries(aeris_real_world_probe PRIVATE AERIS::core)
    target_compile_features(aeris_real_world_probe PRIVATE cxx_std_17)
    set_target_properties(aeris_real_world_probe PROPERTIES CXX_EXTENSIONS OFF)
    aeris_enable_warnings(aeris_real_world_probe)

    add_executable(aeris_real_world_globe_probe tools/real_world_globe_probe.cpp)
    target_link_libraries(aeris_real_world_globe_probe PRIVATE AERIS::core)
    target_compile_features(aeris_real_world_globe_probe PRIVATE cxx_std_17)
    set_target_properties(aeris_real_world_globe_probe PROPERTIES CXX_EXTENSIONS OFF)
    aeris_enable_warnings(aeris_real_world_globe_probe)

    add_executable(aeris_shapefile_projection_probe tools/shapefile_projection_probe.cpp)
    target_link_libraries(aeris_shapefile_projection_probe PRIVATE AERIS::core)
    target_compile_features(aeris_shapefile_projection_probe PRIVATE cxx_std_17)
    set_target_properties(aeris_shapefile_projection_probe PROPERTIES CXX_EXTENSIONS OFF)
    aeris_enable_warnings(aeris_shapefile_projection_probe)
endif()

if(AERIS_BUILD_TESTS)
    include(CTest)
    enable_testing()
    add_subdirectory(tests)
endif()
