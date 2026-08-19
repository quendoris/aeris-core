# SPDX-FileCopyrightText: 2026 quendoris
# SPDX-License-Identifier: AGPL-3.0-only

cmake_minimum_required(VERSION 3.16)
project(aeris VERSION 0.1.0 DESCRIPTION "Authalic Equal-area Rendering & Ingestion System" LANGUAGES CXX)

option(AERIS_BUILD_TESTS "Build AERIS tests" ON)
option(AERIS_BUILD_TOOLS "Build AERIS development tools" ON)
# Storage remains an opt-in native layer: SQLite is private to AERIS::storage,
# and canonical project geometry remains a storage-native DTO/encoding rather
# than a dependency on the in-memory source or rendering model.
option(AERIS_BUILD_STORAGE "Build SQLite-backed AERIS project/session storage" OFF)
# Project orchestration is the explicit verified composition layer over core/source + storage.
# Enabling it must never make SQLite a dependency of AERIS::core itself; configure and
# Project CI both enforce that storage is explicitly enabled for this composition layer.
option(AERIS_BUILD_PROJECT "Build AERIS project orchestration over core and storage" OFF)
# Keep the GUI opt-in so the reference core never acquires a transitive Qt
# requirement merely because a frontend exists in the same repository.
option(AERIS_BUILD_VIEWER "Build the optional Qt Widgets AERIS viewer" OFF)
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

# SHA-256 is a small runtime-neutral support primitive shared by source
# acquisition and durable project resources. Keeping it outside aeris_core lets
# AERIS::storage verify embedded bytes without acquiring a dependency on the
# cartographic core.
add_library(aeris_hash STATIC src/util/sha256.cpp)
add_library(AERIS::hash ALIAS aeris_hash)
target_include_directories(aeris_hash PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
target_compile_features(aeris_hash PUBLIC cxx_std_17)
set_target_properties(aeris_hash PROPERTIES CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
aeris_enable_warnings(aeris_hash)

add_library(aeris_core STATIC
    src/geo/rotation.cpp
    src/geo/wgs84.cpp
    src/geometry/geographic.cpp
    src/geometry/planar.cpp
    src/projection/adapter.cpp
    src/projection/primitives.cpp
    src/projection/ring.cpp
    src/projection/ring_piecewise.cpp
    src/projection/seam.cpp
    src/projection/subdivide.cpp
    src/projection/wgs84.cpp
    src/source/acquisition.cpp
    src/source/adapter.cpp
    src/source/dbf.cpp
    src/source/natural_earth.cpp
    src/source/registry.cpp
    src/source/shapefile.cpp
    src/view/globe.cpp
    src/view/globe_curve.cpp
    src/view/globe_polygon.cpp
    src/view/globe_polygon_verified.cpp
)
add_library(AERIS::core ALIAS aeris_core)
target_include_directories(aeris_core PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
target_compile_features(aeris_core PUBLIC cxx_std_17)
set_target_properties(aeris_core PROPERTIES CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
target_link_libraries(aeris_core PUBLIC AERIS::hash)
aeris_enable_warnings(aeris_core)

if(AERIS_BUILD_STORAGE)
    find_package(SQLite3 REQUIRED)
    if(TARGET SQLite3::SQLite3)
        set(AERIS_SQLITE_TARGET SQLite3::SQLite3)
    elseif(TARGET SQLite::SQLite3)
        set(AERIS_SQLITE_TARGET SQLite::SQLite3)
    else()
        add_library(aeris_sqlite3_dependency INTERFACE)
        target_include_directories(aeris_sqlite3_dependency INTERFACE ${SQLite3_INCLUDE_DIRS})
        target_link_libraries(aeris_sqlite3_dependency INTERFACE ${SQLite3_LIBRARIES})
        set(AERIS_SQLITE_TARGET aeris_sqlite3_dependency)
    endif()

    add_library(aeris_storage STATIC
        src/storage/sqlite_detail.cpp
        src/storage/project.cpp
        src/storage/session.cpp
        src/storage/provenance.cpp
        src/storage/geometry.cpp
        src/storage/dataset.cpp
        src/storage/feature_property.cpp
        src/storage/resource.cpp
        src/storage/layer.cpp
        src/storage/layer_stack.cpp
        src/storage/style.cpp
        src/storage/projection.cpp
    )
    add_library(AERIS::storage ALIAS aeris_storage)
    target_include_directories(aeris_storage
        PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/storage
    )
    target_compile_features(aeris_storage PUBLIC cxx_std_17)
    set_target_properties(aeris_storage PROPERTIES CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
    target_link_libraries(aeris_storage PRIVATE ${AERIS_SQLITE_TARGET} AERIS::hash)
    aeris_enable_warnings(aeris_storage)
endif()

if(AERIS_BUILD_PROJECT)
    if(NOT AERIS_BUILD_STORAGE)
        message(FATAL_ERROR "AERIS_BUILD_PROJECT requires AERIS_BUILD_STORAGE=ON")
    endif()

    add_library(aeris_project STATIC
        src/project/source_bridge.cpp
        src/project/world_layers.cpp
    )
    add_library(AERIS::project ALIAS aeris_project)
    target_include_directories(aeris_project PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
    target_compile_features(aeris_project PUBLIC cxx_std_17)
    set_target_properties(aeris_project PROPERTIES CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
    target_link_libraries(aeris_project PUBLIC AERIS::core AERIS::storage)
    aeris_enable_warnings(aeris_project)
endif()

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

    add_executable(aeris_real_world_globe_fill_probe tools/real_world_globe_fill_probe.cpp)
    target_link_libraries(aeris_real_world_globe_fill_probe PRIVATE AERIS::core)
    target_compile_features(aeris_real_world_globe_fill_probe PRIVATE cxx_std_17)
    set_target_properties(aeris_real_world_globe_fill_probe PROPERTIES CXX_EXTENSIONS OFF)
    aeris_enable_warnings(aeris_real_world_globe_fill_probe)

    add_executable(aeris_shapefile_projection_probe tools/shapefile_projection_probe.cpp)
    target_link_libraries(aeris_shapefile_projection_probe PRIVATE AERIS::core)
    target_compile_features(aeris_shapefile_projection_probe PRIVATE cxx_std_17)
    set_target_properties(aeris_shapefile_projection_probe PROPERTIES CXX_EXTENSIONS OFF)
    aeris_enable_warnings(aeris_shapefile_projection_probe)

    add_executable(aeris_natural_earth_admin0_probe tools/natural_earth_admin0_probe.cpp)
    target_link_libraries(aeris_natural_earth_admin0_probe PRIVATE AERIS::core)
    target_compile_features(aeris_natural_earth_admin0_probe PRIVATE cxx_std_17)
    set_target_properties(aeris_natural_earth_admin0_probe PROPERTIES CXX_EXTENSIONS OFF)
    aeris_enable_warnings(aeris_natural_earth_admin0_probe)

    if(TARGET aeris_project)
        add_executable(aeris_project_natural_earth_probe tools/project_natural_earth_probe.cpp)
        target_link_libraries(aeris_project_natural_earth_probe PRIVATE AERIS::project)
        target_compile_features(aeris_project_natural_earth_probe PRIVATE cxx_std_17)
        set_target_properties(aeris_project_natural_earth_probe PROPERTIES CXX_EXTENSIONS OFF)
        aeris_enable_warnings(aeris_project_natural_earth_probe)

        add_executable(aeris_project_natural_earth_admin0_probe tools/project_natural_earth_admin0_probe.cpp)
        target_link_libraries(aeris_project_natural_earth_admin0_probe PRIVATE AERIS::project)
        target_compile_features(aeris_project_natural_earth_admin0_probe PRIVATE cxx_std_17)
        set_target_properties(aeris_project_natural_earth_admin0_probe PROPERTIES CXX_EXTENSIONS OFF)
        aeris_enable_warnings(aeris_project_natural_earth_admin0_probe)
    endif()
endif()

if(AERIS_BUILD_VIEWER)
    add_subdirectory(viewer)
endif()

if(AERIS_BUILD_TESTS)
    include(CTest)
    enable_testing()
    add_subdirectory(tests)
endif()
