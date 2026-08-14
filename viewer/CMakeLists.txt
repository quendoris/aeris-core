# SPDX-FileCopyrightText: 2026 quendoris
# SPDX-License-Identifier: AGPL-3.0-only

find_package(Qt6 REQUIRED COMPONENTS Widgets)

add_executable(aeris_viewer
    main.cpp
    main_window.cpp
    map_canvas.cpp
    scene_builder.cpp
    scene_controller.cpp
    world_loader.cpp
)

target_link_libraries(aeris_viewer PRIVATE AERIS::core Qt6::Widgets)
target_compile_features(aeris_viewer PRIVATE cxx_std_17)
set_target_properties(aeris_viewer PROPERTIES
    CXX_EXTENSIONS OFF
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON
)
aeris_enable_warnings(aeris_viewer)
