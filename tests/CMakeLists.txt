# tests/CMakeLists.txt

add_executable(test_synthetic_x86
    test_synthetic_x86.cpp
)
target_include_directories(test_synthetic_x86 PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
)

add_executable(test_controller_mapping
    test_controller_mapping.cpp
)
target_include_directories(test_controller_mapping PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
)

add_executable(test_raster_renderer
    test_raster_renderer.cpp
)
target_include_directories(test_raster_renderer PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
)

add_executable(test_stereo_ceiling_parallax
    test_stereo_ceiling_parallax.cpp
)
target_include_directories(test_stereo_ceiling_parallax PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
)

add_executable(test_timing_decoupling_g2
    test_timing_decoupling_g2.cpp
)
target_include_directories(test_timing_decoupling_g2 PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
)

add_executable(test_hook_safety
    test_hook_safety.cpp
)
target_include_directories(test_hook_safety PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
)

add_executable(test_snapshot_lifecycle
    test_snapshot_lifecycle.cpp
)
target_include_directories(test_snapshot_lifecycle PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
)

add_executable(test_stereo_transport_stability
    test_stereo_transport_stability.cpp
)
target_include_directories(test_stereo_transport_stability PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
)

add_executable(test_dual_frustum_visibility
    test_dual_frustum_visibility.cpp
)
target_include_directories(test_dual_frustum_visibility PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
)

add_executable(test_headset_lifecycle
    test_headset_lifecycle.cpp
)
target_include_directories(test_headset_lifecycle PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
)

add_executable(test_ceiling_and_camera
    test_ceiling_and_camera.cpp
)
target_include_directories(test_ceiling_and_camera PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
)

add_executable(test_hud_and_style
    test_hud_and_style.cpp
)
target_include_directories(test_hud_and_style PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
)

add_executable(test_performance_and_thermal
    test_performance_and_thermal.cpp
)
target_include_directories(test_performance_and_thermal PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
)

add_executable(test_savegame_security
    test_savegame_security.cpp
)
target_include_directories(test_savegame_security PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
)

add_executable(test_passthrough_and_anchors
    test_passthrough_and_anchors.cpp
)
target_include_directories(test_passthrough_and_anchors PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
)

add_executable(test_modern_input
    test_modern_input.cpp
)
target_include_directories(test_modern_input PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
)

# Enable testing
enable_testing()
add_test(NAME test_synthetic_x86 COMMAND test_synthetic_x86)
add_test(NAME test_controller_mapping COMMAND test_controller_mapping)
add_test(NAME test_raster_renderer COMMAND test_raster_renderer)
add_test(NAME test_stereo_ceiling_parallax COMMAND test_stereo_ceiling_parallax)
add_test(NAME test_timing_decoupling_g2 COMMAND test_timing_decoupling_g2)
add_test(NAME test_hook_safety COMMAND test_hook_safety)
add_test(NAME test_snapshot_lifecycle COMMAND test_snapshot_lifecycle)
add_test(NAME test_stereo_transport_stability COMMAND test_stereo_transport_stability)
add_test(NAME test_dual_frustum_visibility COMMAND test_dual_frustum_visibility)
add_test(NAME test_headset_lifecycle COMMAND test_headset_lifecycle)
add_test(NAME test_ceiling_and_camera COMMAND test_ceiling_and_camera)
add_test(NAME test_hud_and_style COMMAND test_hud_and_style)
add_test(NAME test_performance_and_thermal COMMAND test_performance_and_thermal)
add_test(NAME test_savegame_security COMMAND test_savegame_security)
add_test(NAME test_passthrough_and_anchors COMMAND test_passthrough_and_anchors)
add_test(NAME test_modern_input COMMAND test_modern_input)

add_executable(test_xrapi_udp test_xrapi_udp.cpp)
add_test(NAME test_xrapi_udp COMMAND test_xrapi_udp)
set_tests_properties(test_xrapi_udp PROPERTIES
    TIMEOUT 10
    RUN_SERIAL TRUE
    LABELS "loopback;no-headset"
)

# The tests must keep their assertions in Release as well as Debug.
add_executable(test_ceiling_mask test_ceiling_mask.cpp)
add_test(NAME test_ceiling_mask COMMAND test_ceiling_mask)
add_executable(test_walk_out test_walk_out.cpp)
add_test(NAME test_walk_out COMMAND test_walk_out)
add_executable(test_modern_controls test_modern_controls.cpp)
add_test(NAME test_modern_controls COMMAND test_modern_controls)
add_executable(test_ceiling_geometry test_ceiling_geometry.cpp)
add_test(NAME test_ceiling_geometry COMMAND test_ceiling_geometry)
add_executable(test_gamepad_bridge test_gamepad_bridge.cpp ../src/mod/gamepad_bridge.cpp)
target_link_libraries(test_gamepad_bridge PRIVATE user32)
add_test(NAME test_gamepad_bridge COMMAND test_gamepad_bridge)
add_executable(test_placement_controls test_placement_controls.cpp
    ../src/mod/placement_controls.cpp ../src/mod/gamepad_bridge.cpp)
target_link_libraries(test_placement_controls PRIVATE user32)
add_test(NAME test_placement_controls COMMAND test_placement_controls)
add_executable(test_spatial_batches test_spatial_batches.cpp)
target_include_directories(test_spatial_batches PRIVATE ../third_party/gta2-renderer/src)
add_test(NAME test_spatial_batches COMMAND test_spatial_batches)
add_executable(test_room_surface test_room_surface.cpp)
add_test(NAME test_room_surface COMMAND test_room_surface)
add_executable(test_room_bridge test_room_bridge.cpp ../src/mod/room_bridge.cpp)
add_test(NAME test_room_bridge COMMAND test_room_bridge)
add_executable(test_texture_filter test_texture_filter.cpp ../src/mod/texture_filter.cpp)
add_test(NAME test_texture_filter COMMAND test_texture_filter)
if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 4)
    add_executable(test_texture_store test_texture_store.cpp
        ../third_party/gta2-renderer/dll/src/texture_store.cpp)
    target_include_directories(test_texture_store PRIVATE
        ../third_party/gta2-renderer/dll/src ../src/mod)
    add_test(NAME test_texture_store COMMAND test_texture_store)
    add_executable(test_d3d9_quality test_d3d9_quality.cpp
        ../third_party/gta2-renderer/src/renderer.cpp
        ../third_party/gta2-renderer/src/camera.cpp ../src/mod/texture_filter.cpp
        ../src/mod/ceiling_mask.cpp)
    target_include_directories(test_d3d9_quality PRIVATE
        ../third_party/gta2-renderer/src ../src/mod)
    target_link_libraries(test_d3d9_quality PRIVATE user32)
    add_test(NAME test_d3d9_quality COMMAND test_d3d9_quality)
    set_tests_properties(test_d3d9_quality PROPERTIES SKIP_RETURN_CODE 77 LABELS "local-gpu;no-headset")
    add_executable(test_live_geometry_lamps test_live_geometry_lamps.cpp
        ../third_party/gta2-renderer/dll/src/live_geometry.cpp)
    target_include_directories(test_live_geometry_lamps PRIVATE
        ../third_party/gta2-renderer/dll/src)
    add_test(NAME test_live_geometry_lamps COMMAND test_live_geometry_lamps)
    add_executable(test_raster_synthetic_lights test_raster_synthetic_lights.cpp
        ../third_party/gta2-renderer/dll/src/synthetic_lights.cpp)
    target_include_directories(test_raster_synthetic_lights PRIVATE
        ../third_party/gta2-renderer/dll/src ../third_party/gta2-renderer/dll/deps/bridge_api)
    target_compile_definitions(test_raster_synthetic_lights PRIVATE GTA2CEILING_RASTER_ONLY=1)
    add_test(NAME test_raster_synthetic_lights COMMAND test_raster_synthetic_lights)
endif()
get_property(gta2ceiling_test_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(test_target IN LISTS gta2ceiling_test_targets)
    target_link_libraries(${test_target} PRIVATE gta2ceiling_core)
    if(MSVC)
        target_compile_options(${test_target} PRIVATE /UNDEBUG)
        set_target_properties(${test_target} PROPERTIES
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        )
    else()
        target_compile_options(${test_target} PRIVATE -UNDEBUG)
    endif()
endforeach()

