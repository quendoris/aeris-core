# SPDX-FileCopyrightText: 2026 quendoris
# SPDX-License-Identifier: AGPL-3.0-only

add_executable(aeris_test_authalic
    test_authalic.cpp
)

target_link_libraries(aeris_test_authalic PRIVATE AERIS::core)
target_compile_features(aeris_test_authalic PRIVATE cxx_std_17)
set_target_properties(aeris_test_authalic PROPERTIES CXX_EXTENSIONS OFF)
aeris_enable_warnings(aeris_test_authalic)
add_test(NAME authalic_math COMMAND aeris_test_authalic)

add_executable(aeris_test_rotation
    test_rotation.cpp
)

target_link_libraries(aeris_test_rotation PRIVATE AERIS::core)
target_compile_features(aeris_test_rotation PRIVATE cxx_std_17)
set_target_properties(aeris_test_rotation PROPERTIES CXX_EXTENSIONS OFF)
aeris_enable_warnings(aeris_test_rotation)
add_test(NAME spherical_rotation COMMAND aeris_test_rotation)

add_executable(aeris_test_projection_primitives
    test_projection_primitives.cpp
)

target_link_libraries(aeris_test_projection_primitives PRIVATE AERIS::core)
target_compile_features(aeris_test_projection_primitives PRIVATE cxx_std_17)
set_target_properties(aeris_test_projection_primitives PROPERTIES CXX_EXTENSIONS OFF)
aeris_enable_warnings(aeris_test_projection_primitives)
add_test(NAME projection_primitives COMMAND aeris_test_projection_primitives)

add_executable(aeris_test_planar_geometry
    test_planar_geometry.cpp
)
target_link_libraries(aeris_test_planar_geometry PRIVATE AERIS::core)
target_compile_features(aeris_test_planar_geometry PRIVATE cxx_std_17)
set_target_properties(aeris_test_planar_geometry PROPERTIES CXX_EXTENSIONS OFF)
aeris_enable_warnings(aeris_test_planar_geometry)
add_test(NAME planar_geometry COMMAND aeris_test_planar_geometry)

add_executable(aeris_test_geographic_area
    test_geographic_area.cpp
)
target_link_libraries(aeris_test_geographic_area PRIVATE AERIS::core)
target_compile_features(aeris_test_geographic_area PRIVATE cxx_std_17)
set_target_properties(aeris_test_geographic_area PROPERTIES CXX_EXTENSIONS OFF)
aeris_enable_warnings(aeris_test_geographic_area)
add_test(NAME geographic_area COMMAND aeris_test_geographic_area)

add_executable(aeris_test_subdivision
    test_subdivision.cpp
)
target_link_libraries(aeris_test_subdivision PRIVATE AERIS::core)
target_compile_features(aeris_test_subdivision PRIVATE cxx_std_17)
set_target_properties(aeris_test_subdivision PROPERTIES CXX_EXTENSIONS OFF)
aeris_enable_warnings(aeris_test_subdivision)
add_test(NAME adaptive_subdivision COMMAND aeris_test_subdivision)

add_executable(aeris_test_polygon_area_invariant
    test_polygon_area_invariant.cpp
)
target_link_libraries(aeris_test_polygon_area_invariant PRIVATE AERIS::core)
target_compile_features(aeris_test_polygon_area_invariant PRIVATE cxx_std_17)
set_target_properties(aeris_test_polygon_area_invariant PROPERTIES CXX_EXTENSIONS OFF)
aeris_enable_warnings(aeris_test_polygon_area_invariant)
add_test(NAME polygon_area_invariant COMMAND aeris_test_polygon_area_invariant)

add_executable(aeris_test_ring_projection_budget
    test_ring_projection_budget.cpp
)
target_link_libraries(aeris_test_ring_projection_budget PRIVATE AERIS::core)
target_compile_features(aeris_test_ring_projection_budget PRIVATE cxx_std_17)
set_target_properties(aeris_test_ring_projection_budget PROPERTIES CXX_EXTENSIONS OFF)
aeris_enable_warnings(aeris_test_ring_projection_budget)
add_test(NAME ring_projection_budget COMMAND aeris_test_ring_projection_budget)

add_executable(aeris_test_globe_view
    test_globe_view.cpp
)
target_link_libraries(aeris_test_globe_view PRIVATE AERIS::core)
target_compile_features(aeris_test_globe_view PRIVATE cxx_std_17)
set_target_properties(aeris_test_globe_view PROPERTIES CXX_EXTENSIONS OFF)
aeris_enable_warnings(aeris_test_globe_view)
add_test(NAME globe_view COMMAND aeris_test_globe_view)

if(TARGET aeris_diagnostic_svg)
    add_test(
        NAME diagnostic_svg_smoke
        COMMAND aeris_diagnostic_svg ${CMAKE_CURRENT_BINARY_DIR}/aeris-diagnostic.svg
    )
endif()
