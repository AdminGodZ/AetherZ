cmake_minimum_required(VERSION 4.3)

project(AetherZ VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(AETHERZ_NATIVE "Optimize for the host CPU" ON)
option(AETHERZ_LTO "Enable link-time optimization" ON)
# The network the engine plays with unless EvalFile says otherwise. Configure
# with -DAETHERZ_EVALFILE= for an engine that uses the classical evaluation.
set(AETHERZ_EVALFILE "${CMAKE_CURRENT_SOURCE_DIR}/networks/aetherz-v2c2-w30.bin" CACHE FILEPATH
    "Bullet quantised.bin to build into the engine as its default network")
# The engine loads (768 -> N)x2 -> 1 networks of one hidden size N, fixed at
# compile time. Empty: the size of the built-in network, or 256 without one.
set(AETHERZ_NNUE_HIDDEN "" CACHE STRING "NNUE hidden layer size (empty: from AETHERZ_EVALFILE)")

if(AETHERZ_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT aetherz_ipo_supported OUTPUT aetherz_ipo_output)
    if(aetherz_ipo_supported)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    endif()
endif()

# Windows gives the main thread only 1 MB (MSVC) or 2 MB (MinGW) of stack.
# Searches started from the command line (bench, tests, tools) run there.
if(WIN32)
    if(MSVC)
        add_link_options(/STACK:16777216)
    else()
        add_link_options(-Wl,--stack,16777216)
    endif()
endif()

add_executable(aetherz
    src/main.cpp
)

add_library(aetherz_engine
    src/engine/attacks.cpp
    src/engine/benchmark.cpp
    src/engine/board.cpp
    src/engine/datagen.cpp
    src/engine/epd.cpp
    src/engine/evaluate.cpp
    src/engine/nnue/embedded.cpp
    src/engine/nnue/features.cpp
    src/engine/nnue/network.cpp
    src/engine/nnue/nnue.cpp
    src/engine/perft.cpp
    src/engine/search.cpp
    src/engine/see.cpp
    src/engine/thread.cpp
    src/engine/types.cpp
    src/engine/uci.cpp
)

target_include_directories(aetherz_engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# The network becomes a byte array in embedded.cpp. The generated file only
# changes (and recompiles) when the network does.
if(AETHERZ_EVALFILE)
    if(NOT EXISTS "${AETHERZ_EVALFILE}")
        message(FATAL_ERROR "AETHERZ_EVALFILE: ${AETHERZ_EVALFILE} does not exist")
    endif()
    file(READ "${AETHERZ_EVALFILE}" aetherz_network_hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," aetherz_network_bytes "${aetherz_network_hex}")
    file(CONFIGURE
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_network.inc"
        CONTENT "${aetherz_network_bytes}\n")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${AETHERZ_EVALFILE}")
    # A network file holds hidden * (768 + 3) + 1 int16 values, padded by
    # Bullet to a multiple of 64 bytes.
    file(SIZE "${AETHERZ_EVALFILE}" aetherz_network_size)
    math(EXPR aetherz_network_hidden "(${aetherz_network_size} / 2 - 1) / 771")
    if(AETHERZ_NNUE_HIDDEN STREQUAL "")
        set(aetherz_nnue_hidden ${aetherz_network_hidden})
    elseif(NOT AETHERZ_NNUE_HIDDEN EQUAL aetherz_network_hidden)
        message(FATAL_ERROR "AETHERZ_NNUE_HIDDEN is ${AETHERZ_NNUE_HIDDEN}, but ${AETHERZ_EVALFILE} "
            "looks like a network with ${aetherz_network_hidden} hidden neurons")
    endif()
    set_source_files_properties(src/engine/nnue/embedded.cpp PROPERTIES
        COMPILE_DEFINITIONS AETHERZ_EMBEDDED_NETWORK
        INCLUDE_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR}/generated")
    message(STATUS "Built-in network: ${AETHERZ_EVALFILE}")
endif()
if(NOT DEFINED aetherz_nnue_hidden)
    if(AETHERZ_NNUE_HIDDEN STREQUAL "")
        set(aetherz_nnue_hidden 256)
    else()
        set(aetherz_nnue_hidden ${AETHERZ_NNUE_HIDDEN})
    endif()
endif()
if(NOT aetherz_nnue_hidden MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "the NNUE hidden size must be a positive multiple of 16, not ${aetherz_nnue_hidden}")
endif()
math(EXPR aetherz_nnue_hidden_remainder "${aetherz_nnue_hidden} % 16")
if(aetherz_nnue_hidden LESS 16 OR NOT aetherz_nnue_hidden_remainder EQUAL 0)
    message(FATAL_ERROR "the NNUE hidden size must be a positive multiple of 16, not ${aetherz_nnue_hidden}")
endif()
target_compile_definitions(aetherz_engine PUBLIC AETHERZ_NNUE_HIDDEN=${aetherz_nnue_hidden})
message(STATUS "NNUE hidden size: ${aetherz_nnue_hidden}")

find_package(Threads REQUIRED)
target_link_libraries(aetherz_engine PUBLIC Threads::Threads)

# MSVC (and clang-cl) already optimize Release builds with /O2 and have no
# native-CPU switch; the optimization flags below are for GCC and Clang,
# including MinGW and Clang builds on Windows.
if(MSVC)
    target_compile_options(aetherz_engine PRIVATE /W4)
else()
    target_compile_options(aetherz_engine PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )

    target_compile_options(aetherz_engine PUBLIC
        $<$<CONFIG:Release>:-O3>
    )

    if(AETHERZ_NATIVE)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64|ARM64")
            target_compile_options(aetherz_engine PUBLIC -mcpu=native)
        else()
            target_compile_options(aetherz_engine PUBLIC -march=native)
        endif()
    endif()
endif()

target_link_libraries(aetherz PRIVATE aetherz_engine)

# The match runner drives engines through POSIX pipes and processes. On
# Windows, build it under WSL or use fastchess/cutechess instead.
if(NOT WIN32)
    add_executable(aetherz_match
        src/tools/match.cpp
    )

    target_link_libraries(aetherz_match PRIVATE aetherz_engine Threads::Threads)
endif()

add_executable(aetherz_tune
    src/tools/tune.cpp
)

target_link_libraries(aetherz_tune PRIVATE aetherz_engine Threads::Threads)

include(CTest)

if(BUILD_TESTING)
    add_executable(aetherz_tests
        tests/engine_tests.cpp
    )

    target_link_libraries(aetherz_tests PRIVATE aetherz_engine)

    add_test(NAME aetherz_tests COMMAND aetherz_tests)
endif()
