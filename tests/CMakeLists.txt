# SPDX-FileCopyrightText: 2026 quendoris
# SPDX-License-Identifier: AGPL-3.0-only

function(aeris_add_core_test target_name source_file test_name)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE AERIS::core)
    target_compile_features(${target_name} PRIVATE cxx_std_17)
    set_target_properties(${target_name} PROPERTIES CXX_EXTENSIONS OFF)
    aeris_enable_warnings(${target_name})
    add_test(NAME ${test_name} COMMAND ${target_name})
endfunction()

function(aeris_add_storage_test target_name source_file test_name)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE AERIS::storage)
    target_compile_features(${target_name} PRIVATE cxx_std_17)
    set_target_properties(${target_name} PROPERTIES CXX_EXTENSIONS OFF)
    aeris_enable_warnings(${target_name})
    add_test(NAME ${test_name} COMMAND ${target_name})
endfunction()

function(aeris_add_project_test target_name source_file test_name)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE AERIS::project)
    target_compile_features(${target_name} PRIVATE cxx_std_17)
    set_target_properties(${target_name} PROPERTIES CXX_EXTENSIONS OFF)
    aeris_enable_warnings(${target_name})
    add_test(NAME ${test_name} COMMAND ${target_name})
endfunction()

aeris_add_core_test(aeris_test_authalic test_authalic.cpp authalic_math)
aeris_add_core_test(aeris_test_rotation test_rotation.cpp spherical_rotation)
aeris_add_core_test(aeris_test_projection_primitives test_projection_primitives.cpp projection_primitives)
aeris_add_core_test(aeris_test_sinu_mollweide test_sinu_mollweide.cpp sinu_mollweide_projection)
aeris_add_core_test(aeris_test_sinu_mollweide_ring test_sinu_mollweide_ring.cpp sinu_mollweide_ring)
aeris_add_core_test(aeris_test_planar_geometry test_planar_geometry.cpp planar_geometry)
aeris_add_core_test(aeris_test_geographic_area test_geographic_area.cpp geographic_area)
aeris_add_core_test(aeris_test_elevation_grid test_elevation_grid.cpp elevation_grid)
aeris_add_core_test(aeris_test_subdivision test_subdivision.cpp adaptive_subdivision)
aeris_add_core_test(aeris_test_polygon_area_invariant test_polygon_area_invariant.cpp polygon_area_invariant)
aeris_add_core_test(aeris_test_ring_projection_budget test_ring_projection_budget.cpp ring_projection_budget)
aeris_add_core_test(aeris_test_polar_seam_roundoff test_polar_seam_roundoff.cpp polar_seam_roundoff)
aeris_add_core_test(aeris_test_polar_movable_seam test_polar_movable_seam.cpp polar_movable_seam)
aeris_add_core_test(aeris_test_seam_split test_seam_split.cpp projection_seam_split)
aeris_add_core_test(aeris_test_piecewise_polygon_semantics test_piecewise_polygon_semantics.cpp piecewise_polygon_semantics)
aeris_add_core_test(aeris_test_piecewise_options test_piecewise_options.cpp piecewise_options)
aeris_add_core_test(aeris_test_source_adapter test_source_adapter.cpp source_adapter_contract)
aeris_add_core_test(aeris_test_acquisition test_acquisition.cpp source_acquisition)
aeris_add_core_test(aeris_test_registry test_registry.cpp source_registry)
aeris_add_core_test(aeris_test_shapefile test_shapefile.cpp shapefile_reader)
aeris_add_core_test(aeris_test_dbf test_dbf.cpp dbf_reader)
aeris_add_core_test(aeris_test_sha256 test_sha256.cpp sha256)
aeris_add_core_test(aeris_test_natural_earth test_natural_earth.cpp natural_earth_adapter)
aeris_add_core_test(aeris_test_globe_view test_globe_view.cpp globe_view)
aeris_add_core_test(aeris_test_globe_curve test_globe_curve.cpp globe_curve)
aeris_add_core_test(aeris_test_globe_polygon test_globe_polygon.cpp globe_polygon)
aeris_add_core_test(aeris_test_globe_polygon_winding test_globe_polygon_winding.cpp globe_polygon_winding)
aeris_add_core_test(aeris_test_globe_polygon_verified test_globe_polygon_verified.cpp globe_polygon_verified)
aeris_add_core_test(aeris_test_scene_geometry test_scene_geometry.cpp scene_geometry)
aeris_add_core_test(aeris_test_surface_geometry test_surface_geometry.cpp surface_geometry)
aeris_add_core_test(aeris_test_surface_inverse test_surface_inverse.cpp surface_inverse)
aeris_add_core_test(aeris_test_surface_pick test_surface_pick.cpp surface_pick)
aeris_add_core_test(aeris_test_projection_catalog test_projection_catalog.cpp projection_catalog)
aeris_add_core_test(aeris_test_standard_projection_cut_scene test_standard_projection_cut_scene.cpp standard_projection_cut_scene)

if(TARGET aeris_storage)
    aeris_add_storage_test(aeris_test_storage test_storage.cpp project_storage)
    aeris_add_storage_test(aeris_test_provenance test_provenance.cpp project_storage_provenance)
    aeris_add_storage_test(aeris_test_geometry_storage test_geometry_storage.cpp project_storage_geometry)
    aeris_add_storage_test(
        aeris_test_geometry_corruption
        test_geometry_corruption.cpp
        project_storage_geometry_corruption
    )
    target_link_libraries(aeris_test_geometry_corruption PRIVATE ${AERIS_SQLITE_TARGET})

    aeris_add_storage_test(
        aeris_test_dataset_atomicity
        test_dataset_atomicity.cpp
        project_storage_dataset_atomicity
    )
    target_link_libraries(aeris_test_dataset_atomicity PRIVATE ${AERIS_SQLITE_TARGET})

    aeris_add_storage_test(
        aeris_test_dataset_properties_atomicity
        test_dataset_properties_atomicity.cpp
        project_storage_dataset_properties_atomicity
    )
    target_link_libraries(aeris_test_dataset_properties_atomicity PRIVATE ${AERIS_SQLITE_TARGET})

    aeris_add_storage_test(
        aeris_test_feature_property
        test_feature_property.cpp
        project_storage_feature_property
    )
    target_link_libraries(aeris_test_feature_property PRIVATE ${AERIS_SQLITE_TARGET})

    aeris_add_storage_test(
        aeris_test_resource
        test_resource.cpp
        project_storage_resource
    )
    target_link_libraries(aeris_test_resource PRIVATE AERIS::hash ${AERIS_SQLITE_TARGET})

    aeris_add_storage_test(
        aeris_test_layer
        test_layer.cpp
        project_storage_layer
    )
    target_link_libraries(aeris_test_layer PRIVATE AERIS::hash ${AERIS_SQLITE_TARGET})

    aeris_add_storage_test(
        aeris_test_layer_stack_atomic
        test_layer_stack_atomic.cpp
        project_storage_layer_stack_atomic
    )
    target_link_libraries(aeris_test_layer_stack_atomic PRIVATE AERIS::hash ${AERIS_SQLITE_TARGET})

    aeris_add_storage_test(
        aeris_test_style
        test_style.cpp
        project_storage_style
    )
    target_link_libraries(aeris_test_style PRIVATE AERIS::hash ${AERIS_SQLITE_TARGET})

    aeris_add_storage_test(
        aeris_test_projection_storage
        test_projection_storage.cpp
        project_storage_projection
    )
    target_link_libraries(aeris_test_projection_storage PRIVATE ${AERIS_SQLITE_TARGET})

    add_executable(aeris_storage_abrupt storage_abrupt.cpp)
    target_link_libraries(aeris_storage_abrupt PRIVATE AERIS::storage)
    target_compile_features(aeris_storage_abrupt PRIVATE cxx_std_17)
    set_target_properties(aeris_storage_abrupt PROPERTIES CXX_EXTENSIONS OFF)
    aeris_enable_warnings(aeris_storage_abrupt)

    set(AERIS_ABRUPT_PROJECT "${CMAKE_CURRENT_BINARY_DIR}/abrupt-storage/world.aeris")
    add_test(NAME project_storage_abrupt_write COMMAND aeris_storage_abrupt write ${AERIS_ABRUPT_PROJECT})
    set_tests_properties(project_storage_abrupt_write PROPERTIES FIXTURES_SETUP aeris_storage_abrupt_fixture)
    add_test(NAME project_storage_abrupt_verify COMMAND aeris_storage_abrupt verify ${AERIS_ABRUPT_PROJECT})
    set_tests_properties(project_storage_abrupt_verify PROPERTIES FIXTURES_REQUIRED aeris_storage_abrupt_fixture)

    find_package(Threads REQUIRED)
    add_executable(aeris_storage_race storage_race.cpp)
    target_link_libraries(aeris_storage_race PRIVATE AERIS::storage Threads::Threads)
    target_compile_features(aeris_storage_race PRIVATE cxx_std_17)
    set_target_properties(aeris_storage_race PROPERTIES CXX_EXTENSIONS OFF)
    aeris_enable_warnings(aeris_storage_race)
    add_test(NAME project_storage_race COMMAND aeris_storage_race)

    add_executable(aeris_layer_stack_race layer_stack_race.cpp)
    target_link_libraries(aeris_layer_stack_race PRIVATE AERIS::storage Threads::Threads)
    target_compile_features(aeris_layer_stack_race PRIVATE cxx_std_17)
    set_target_properties(aeris_layer_stack_race PROPERTIES CXX_EXTENSIONS OFF)
    aeris_enable_warnings(aeris_layer_stack_race)
    add_test(NAME project_storage_layer_stack_race COMMAND aeris_layer_stack_race)

    add_executable(aeris_provenance_race provenance_race.cpp)
    target_link_libraries(aeris_provenance_race PRIVATE AERIS::storage Threads::Threads)
    target_compile_features(aeris_provenance_race PRIVATE cxx_std_17)
    set_target_properties(aeris_provenance_race PROPERTIES CXX_EXTENSIONS OFF)
    aeris_enable_warnings(aeris_provenance_race)
    add_test(NAME project_storage_provenance_race COMMAND aeris_provenance_race)
endif()

if(TARGET aeris_project)
    aeris_add_project_test(
        aeris_test_project_source_bridge
        test_project_source_bridge.cpp
        project_source_bridge
    )
    aeris_add_project_test(
        aeris_test_project_feature_property_bridge
        test_project_feature_property_bridge.cpp
        project_feature_property_bridge
    )
    aeris_add_project_test(
        aeris_test_project_world_layers
        test_project_world_layers.cpp
        project_world_layers
    )
    aeris_add_project_test(
        aeris_test_project_source_reader
        test_project_source_reader.cpp
        project_source_reader
    )
endif()

if(TARGET aeris_diagnostic_svg)
    add_test(NAME diagnostic_svg_smoke COMMAND aeris_diagnostic_svg ${CMAKE_CURRENT_BINARY_DIR}/aeris-diagnostic.svg)
endif()
