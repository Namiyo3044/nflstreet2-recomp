cmake_minimum_required(VERSION 3.20)
project(nfls2_port C)

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

add_executable(nfls2
    src/main.c
    src/runtime/dol.c
    src/runtime/hle_cpu.c
    src/runtime/hle_exceptions.c
    src/runtime/mmio.c
    src/runtime/sha1.c
    src/runtime/symbols.c)
target_link_libraries(nfls2 PRIVATE nfls2_game)
if(NOT MSVC)
    target_link_libraries(nfls2 PRIVATE m)
endif()
