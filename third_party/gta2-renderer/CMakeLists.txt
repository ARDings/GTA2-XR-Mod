# MIT renderer source from gebdag/gta2-rtx-remix, pinned by PROVENANCE.md.
# No original GTA2 files or Remix runtime/asset packages are included.
set(renderer_sources
    dll/src/dllmain.cpp dll/src/world_view.cpp dll/src/log.cpp
    dll/src/texture_store.cpp dll/src/overlay.cpp dll/src/live_geometry.cpp
    dll/src/ground.cpp dll/src/remix_api.cpp dll/src/remix_lights.cpp
    dll/src/debug_overlay.cpp dll/src/synthetic_lights.cpp dll/src/settings.cpp
    dll/src/frame_limiter.cpp dll/src/time_of_day.cpp dll/src/intro_movie.cpp
    dll/src/hud_pass.cpp dll/src/gibs.cpp dll/src/light_bench.cpp
    dll/src/plugin_d3d6.cpp dll/src/plugin_slot.asm
    dll/deps/imgui/imgui.cpp dll/deps/imgui/imgui_draw.cpp
    dll/deps/imgui/imgui_tables.cpp dll/deps/imgui/imgui_widgets.cpp
    dll/deps/imgui/backends/imgui_impl_dx9.cpp
    dll/deps/imgui/backends/imgui_impl_win32.cpp
    src/camera.cpp src/gta2_map.cpp src/gta2_style.cpp src/renderer.cpp src/world_mesh.cpp
)
add_library(gta2ceiling SHARED ${renderer_sources} dll/gta2dx9.def
    ${CMAKE_SOURCE_DIR}/src/mod/ceiling_bridge.cpp ${CMAKE_SOURCE_DIR}/src/mod/gamepad_bridge.cpp
    ${CMAKE_SOURCE_DIR}/src/mod/ceiling_mask.cpp
    ${CMAKE_SOURCE_DIR}/src/mod/placement_controls.cpp
    ${CMAKE_SOURCE_DIR}/src/mod/texture_filter.cpp
    ${CMAKE_SOURCE_DIR}/src/mod/room_bridge.cpp)
set_source_files_properties(dll/src/plugin_slot.asm PROPERTIES COMPILE_OPTIONS "/safeseh")
target_include_directories(gta2ceiling PRIVATE dll/deps/bridge_api dll/deps/imgui src dll/src ${CMAKE_SOURCE_DIR}/src/mod)
target_link_libraries(gta2ceiling PRIVATE gta2ceiling_core user32 psapi gdi32 dwmapi winmm ws2_32 bcrypt)
target_compile_definitions(gta2ceiling PRIVATE GTA2CEILING_RASTER_ONLY=1)
add_library(gta2ceiling_vid SHARED vid/src/vid_proxy.cpp vid/dmavideo.def)
target_link_libraries(gta2ceiling_vid PRIVATE user32)
set_target_properties(gta2ceiling gta2ceiling_vid PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)
