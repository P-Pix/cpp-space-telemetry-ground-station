cmake_minimum_required(VERSION 3.16)

project(cpp_space_telemetry_ground_station
    VERSION 1.0.0
    DESCRIPTION "C++20 Linux telemetry ground station simulator"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(STGS_BUILD_TESTS "Build unit tests" ON)

find_package(Threads REQUIRED)

set(STGS_WARNINGS -Wall -Wextra -Wpedantic)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    list(APPEND STGS_WARNINGS -Wno-free-nonheap-object)
endif()

add_library(stgs_core
    src/Crc32.cpp
    src/FrameCodec.cpp
    src/Logger.cpp
    src/Replay.cpp)

target_include_directories(stgs_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_compile_options(stgs_core PRIVATE ${STGS_WARNINGS})

add_library(stgs_network
    src/NetworkServer.cpp)

target_link_libraries(stgs_network
    PUBLIC
        stgs_core
        Threads::Threads)

target_compile_options(stgs_network PRIVATE ${STGS_WARNINGS})

add_executable(stgs_ground_station
    apps/ground_station.cpp)

target_link_libraries(stgs_ground_station
    PRIVATE
        stgs_network
        stgs_core
        Threads::Threads)

target_compile_options(stgs_ground_station PRIVATE ${STGS_WARNINGS})

add_executable(stgs_satellite_simulator
    apps/satellite_simulator.cpp)

target_link_libraries(stgs_satellite_simulator
    PRIVATE
        stgs_core
        Threads::Threads)

target_compile_options(stgs_satellite_simulator PRIVATE ${STGS_WARNINGS})

if(STGS_BUILD_TESTS)
    enable_testing()

    add_executable(stgs_tests
        tests/test_main.cpp)

    target_link_libraries(stgs_tests
        PRIVATE
            stgs_core
            Threads::Threads)

    target_compile_options(stgs_tests PRIVATE ${STGS_WARNINGS})

    add_test(NAME stgs_unit_tests COMMAND stgs_tests)
endif()
