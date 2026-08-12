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
