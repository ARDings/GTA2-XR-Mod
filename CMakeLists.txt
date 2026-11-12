cmake_minimum_required(VERSION 3.20)
project(GTA2Ceiling VERSION 0.1.0 LANGUAGES C CXX)

# Enforce x86 (32-bit) build architecture
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "GTA2Ceiling requires a 32-bit (x86) target architecture for GTA2 and Wine WoW64 compatibility! Pass -A Win32 to CMake.")
endif()

# Build outputs stay out of the source tree: under the folder in the
# GTA2CEILING_WORK_ROOT environment variable when it is set, otherwise on D:
# (the default work root is D:/GTA2Ceiling-work, see docs/BUILDING.md).
get_filename_component(GCC_BUILD_DIR "${CMAKE_BINARY_DIR}" ABSOLUTE)
if(DEFINED ENV{GTA2CEILING_WORK_ROOT})
    file(TO_CMAKE_PATH "$ENV{GTA2CEILING_WORK_ROOT}" GCC_WORK_ROOT)
    get_filename_component(GCC_WORK_ROOT "${GCC_WORK_ROOT}" ABSOLUTE)
    string(TOLOWER "${GCC_BUILD_DIR}/" GCC_BUILD_LOWER)
    string(TOLOWER "${GCC_WORK_ROOT}/" GCC_ROOT_LOWER)
    string(FIND "${GCC_BUILD_LOWER}" "${GCC_ROOT_LOWER}" GCC_ROOT_AT)
    if(NOT GCC_ROOT_AT EQUAL 0)
        message(FATAL_ERROR "Build outputs must be under GTA2CEILING_WORK_ROOT (${GCC_WORK_ROOT}). Current build dir: ${CMAKE_BINARY_DIR}")
    endif()
else()
    string(SUBSTRING "${GCC_BUILD_DIR}" 0 2 DRIVE_LETTER)
    string(TOUPPER "${DRIVE_LETTER}" DRIVE_LETTER_UPPER)
    if(NOT DRIVE_LETTER_UPPER STREQUAL "D:")
        message(FATAL_ERROR "Build outputs go to D: by default (e.g. D:/GTA2Ceiling-work/build-x86). Set GTA2CEILING_WORK_ROOT to use another folder. Current build dir: ${CMAKE_BINARY_DIR}")
    endif()
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Configure output directories strictly in the build tree
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

include_directories(
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

# Common compiler options
if(MSVC)
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/W4> $<$<COMPILE_LANGUAGE:CXX>:/EHsc> $<$<COMPILE_LANGUAGE:CXX>:/permissive-> $<$<COMPILE_LANGUAGE:CXX>:/Zc:preprocessor>)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

# Subdirectories
enable_testing()
add_subdirectory(src)
add_subdirectory(tests)
if(MSVC AND WIN32)
    enable_language(ASM_MASM)
    add_subdirectory(third_party/gta2-renderer)
endif()

