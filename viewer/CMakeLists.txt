# SPDX-FileCopyrightText: 2026 quendoris
# SPDX-License-Identifier: AGPL-3.0-only

find_package(Qt6 REQUIRED COMPONENTS Core Widgets)

add_executable(aeris_viewer
    main.cpp
    main_window.cpp
    layer_stack.cpp
    map_canvas.cpp
    render_surface.cpp
    scene_builder.cpp
    scene_controller.cpp
    scene_presentation.cpp
    unfold.cpp
    unfold_controller.cpp
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

add_executable(aeris_viewer_scene_probe
    scene_probe.cpp
    scene_builder.cpp
    scene_presentation.cpp
    unfold.cpp
    world_loader.cpp
)
target_link_libraries(aeris_viewer_scene_probe PRIVATE AERIS::core)
target_compile_features(aeris_viewer_scene_probe PRIVATE cxx_std_17)
set_target_properties(aeris_viewer_scene_probe PROPERTIES CXX_EXTENSIONS OFF)
aeris_enable_warnings(aeris_viewer_scene_probe)

add_executable(aeris_viewer_presentation_probe
    presentation_probe.cpp
    scene_presentation.cpp
)
target_link_libraries(aeris_viewer_presentation_probe PRIVATE AERIS::core)
target_compile_features(aeris_viewer_presentation_probe PRIVATE cxx_std_17)
set_target_properties(aeris_viewer_presentation_probe PROPERTIES CXX_EXTENSIONS OFF)
aeris_enable_warnings(aeris_viewer_presentation_probe)

add_executable(aeris_viewer_controller_probe
    controller_probe.cpp
    scene_builder.cpp
    scene_controller.cpp
    scene_presentation.cpp
    unfold.cpp
    unfold_controller.cpp
    world_loader.cpp
)
target_link_libraries(aeris_viewer_controller_probe PRIVATE AERIS::core Qt6::Core)
target_compile_features(aeris_viewer_controller_probe PRIVATE cxx_std_17)
set_target_properties(aeris_viewer_controller_probe PROPERTIES CXX_EXTENSIONS OFF)
aeris_enable_warnings(aeris_viewer_controller_probe)

add_executable(aeris_viewer_layer_stack_probe
    layer_stack_probe.cpp
    layer_stack.cpp
)
target_link_libraries(aeris_viewer_layer_stack_probe PRIVATE AERIS::core)
target_compile_features(aeris_viewer_layer_stack_probe PRIVATE cxx_std_17)
set_target_properties(aeris_viewer_layer_stack_probe PROPERTIES CXX_EXTENSIONS OFF)
aeris_enable_warnings(aeris_viewer_layer_stack_probe)
