cmake_minimum_required(VERSION 4.3)

project(AetherZ VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(aetherz
    src/main.cpp
)

add_library(aetherz_engine
    src/engine/attacks.cpp
    src/engine/benchmark.cpp
    src/engine/board.cpp
    src/engine/epd.cpp
    src/engine/evaluate.cpp
    src/engine/perft.cpp
    src/engine/search.cpp
    src/engine/see.cpp
    src/engine/types.cpp
    src/engine/uci.cpp
)

target_include_directories(aetherz_engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_options(aetherz_engine PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

target_link_libraries(aetherz PRIVATE aetherz_engine)

include(CTest)

if(BUILD_TESTING)
    add_executable(aetherz_tests
        tests/engine_tests.cpp
    )

    target_link_libraries(aetherz_tests PRIVATE aetherz_engine)

    add_test(NAME aetherz_tests COMMAND aetherz_tests)
endif()
