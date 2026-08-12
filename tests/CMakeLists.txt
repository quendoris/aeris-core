# SPDX-FileCopyrightText: 2026 quendoris
# SPDX-License-Identifier: AGPL-3.0-only

add_executable(aeris_test_authalic
    test_authalic.cpp
)

target_link_libraries(aeris_test_authalic PRIVATE AERIS::core)
target_compile_features(aeris_test_authalic PRIVATE cxx_std_17)
set_target_properties(aeris_test_authalic PROPERTIES CXX_EXTENSIONS OFF)

if(MSVC)
    target_compile_options(aeris_test_authalic PRIVATE /W4 /permissive-)
else()
    target_compile_options(aeris_test_authalic PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
    )
endif()

add_test(NAME authalic_math COMMAND aeris_test_authalic)
