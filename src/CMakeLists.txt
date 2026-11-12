# src/CMakeLists.txt

add_library(gta2ceiling_core STATIC
    adapter/game_adapter.cpp
    renderer/stereo_renderer.cpp
    xr/xr_transport.cpp
    input/input_controller.cpp
)

target_include_directories(gta2ceiling_core PUBLIC
    "${CMAKE_SOURCE_DIR}/include"
)
if(WIN32)
    target_link_libraries(gta2ceiling_core PUBLIC ws2_32)
endif()
set_target_properties(gta2ceiling_core PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

# Standalone runtime probe for WinlatorXR compatibility verification
add_executable(xr_runtime_probe
    tools/xr_runtime_probe.cpp
)

target_include_directories(xr_runtime_probe PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
)
target_link_libraries(xr_runtime_probe PRIVATE gta2ceiling_core)
set_target_properties(xr_runtime_probe PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

if(WIN32)
    add_executable(keyboard_runtime_probe tools/keyboard_runtime_probe.cpp)
    target_link_libraries(keyboard_runtime_probe PRIVATE user32)
    set_target_properties(keyboard_runtime_probe PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

