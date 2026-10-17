cmake_minimum_required(VERSION 3.20)

# Optimised build unless asked otherwise (an empty build type means no
# optimisation at all, which makes the renderer and CPU runtime ~4x slower).
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

project(nfls2_port C)
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Paths to things that live OUTSIDE this repo (never committed here).
set(NFLS2_DOLRECOMP_DIR "C:/nfls2/tools/dolrecomp" CACHE PATH
    "DolRecomp checkout (its src/cpu runtime is compiled into the port)")
set(NFLS2_GENERATED_DIR "C:/nfls2/recomp/dolrecomp/generated" CACHE PATH
    "DolRecomp output for your own main.dol")

if(NOT EXISTS "${NFLS2_DOLRECOMP_DIR}/src/cpu/cpu.c")
    message(FATAL_ERROR "DolRecomp not found at ${NFLS2_DOLRECOMP_DIR} (set NFLS2_DOLRECOMP_DIR)")
endif()
if(NOT EXISTS "${NFLS2_GENERATED_DIR}/generated.h")
    message(FATAL_ERROR "Generated code not found at ${NFLS2_GENERATED_DIR} (run DolRecomp on your main.dol first)")
endif()

file(GLOB NFLS2_CHUNKS CONFIGURE_DEPENDS "${NFLS2_GENERATED_DIR}/chunks/*.c")
list(LENGTH NFLS2_CHUNKS NFLS2_CHUNK_COUNT)
message(STATUS "Recompiled chunks: ${NFLS2_CHUNK_COUNT}")

# Settings shared by everything that includes generated.h.
add_library(nfls2_settings INTERFACE)
target_include_directories(nfls2_settings INTERFACE
    "${NFLS2_DOLRECOMP_DIR}/src"
    "${NFLS2_GENERATED_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(nfls2_settings INTERFACE DOLRECOMP_ENABLE_REPLACEMENTS)
if(MSVC)
    target_compile_definitions(nfls2_settings INTERFACE _CRT_SECURE_NO_WARNINGS)
endif()

# DolRecomp's reference Gekko CPU core (FPU, paired singles, memory access).
add_library(gekko_cpu STATIC "${NFLS2_DOLRECOMP_DIR}/src/cpu/cpu.c")
target_link_libraries(gekko_cpu PUBLIC nfls2_settings)

# The recompiled game code. Huge machine-generated files: no warnings, and a
# lighter optimisation level by default so builds take minutes, not hours.
set(NFLS2_GAME_OPT "-O1" CACHE STRING "Optimisation flag for the recompiled game code")
add_library(nfls2_game STATIC ${NFLS2_CHUNKS})
target_link_libraries(nfls2_game PUBLIC gekko_cpu)
if(CMAKE_C_COMPILER_ID STREQUAL "Clang" AND CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    # clang-cl: /clang: passes the GCC-style flag through (/O1 would mean "minimise size").
    target_compile_options(nfls2_game PRIVATE /w "/clang:${NFLS2_GAME_OPT}")
elseif(CMAKE_C_COMPILER_ID STREQUAL "Clang")
    target_compile_options(nfls2_game PRIVATE -w ${NFLS2_GAME_OPT})
elseif(MSVC)
    target_compile_options(nfls2_game PRIVATE /w /bigobj)
endif()

# Live window (SDL3). Uses an installed SDL3 if CMake can find one, otherwise
# downloads and builds it (static) on the first configure.
option(NFLS2_WINDOW "Build the live window (SDL3)" ON)
if(NFLS2_WINDOW)
    find_package(SDL3 CONFIG QUIET)
    if(NOT SDL3_FOUND)
        message(STATUS "SDL3 not found: downloading and building SDL 3.2.20")
        include(FetchContent)
        set(SDL_SHARED OFF CACHE BOOL "" FORCE)
        set(SDL_STATIC ON CACHE BOOL "" FORCE)
        set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
        set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(SDL3
            GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
            GIT_TAG release-3.2.20
            GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(SDL3)
    endif()
endif()

add_executable(nfls2
    src/main.c
    src/audio/ax_hle.c
    src/gx/fifo.c
    src/gx/raster.c
    src/gx/texture.c
    src/runtime/disc.c
    src/runtime/dol.c
    src/runtime/hle_cpu.c
    src/runtime/hle_exceptions.c
    src/runtime/interrupts.c
    src/runtime/mmio.c
    src/runtime/pad.c
    src/runtime/sha1.c
    src/runtime/symbols.c)
target_link_libraries(nfls2 PRIVATE nfls2_game)
if(NFLS2_WINDOW)
    target_sources(nfls2 PRIVATE src/platform/window.c)
    target_compile_definitions(nfls2 PRIVATE NFLS2_HAVE_WINDOW)
    if(TARGET SDL3::SDL3-static)
        target_link_libraries(nfls2 PRIVATE SDL3::SDL3-static)
    else()
        target_link_libraries(nfls2 PRIVATE SDL3::SDL3)
    endif()
endif()
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(nfls2 PRIVATE Threads::Threads)
endif()
if(NOT MSVC)
    target_link_libraries(nfls2 PRIVATE m)
endif()
