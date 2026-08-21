cmake_minimum_required(VERSION 3.16)

project(cpp_space_telemetry_ground_station
    VERSION 1.2.0
    DESCRIPTION "C++20 Linux telemetry ground station showcase"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(STGS_BUILD_TESTS "Build unit and integration tests" ON)
option(STGS_BUILD_BENCHMARKS "Build benchmark tools" ON)
option(STGS_WARNINGS_AS_ERRORS "Treat project warnings as errors" ON)
option(STGS_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(STGS_ENABLE_THREAD_SANITIZER "Enable ThreadSanitizer" OFF)

find_package(Threads REQUIRED)

set(STGS_WARNINGS
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wconversion
    -Wsign-conversion
    -Wformat=2)

# GCC 14 can emit a false positive in std::vector reallocation under -O3 for this byte codec.
# Keep the warning set strict while disabling only this compiler-specific diagnostic.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    list(APPEND STGS_WARNINGS -Wno-free-nonheap-object)
endif()

if(STGS_WARNINGS_AS_ERRORS)
    list(APPEND STGS_WARNINGS -Werror)
endif()

if(STGS_ENABLE_SANITIZERS AND STGS_ENABLE_THREAD_SANITIZER)
    message(FATAL_ERROR "ASan/UBSan and ThreadSanitizer must use separate builds")
endif()

if(STGS_ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
endif()

if(STGS_ENABLE_THREAD_SANITIZER AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
endif()

add_library(stgs_core
    src/ApplicationPayload.cpp
    src/Crc32.cpp
    src/FrameCodec.cpp
    src/Logger.cpp
    src/Replay.cpp
    src/SignalProcessing.cpp
    src/StationHealth.cpp
    src/TerminalUi.cpp)

target_include_directories(stgs_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_compile_options(stgs_core PRIVATE ${STGS_WARNINGS})

add_library(stgs_network
    src/NetworkServer.cpp
    src/NetworkServerTcp.cpp
    src/NetworkServerUdp.cpp
    src/PortDiagnostics.cpp)

target_link_libraries(stgs_network
    PUBLIC
        stgs_core
        Threads::Threads)

target_compile_options(stgs_network PRIVATE ${STGS_WARNINGS})

add_executable(stgs_ground_station
    apps/ground_station.cpp
    apps/ground_station/GroundStationApp.cpp
    apps/ground_station/GroundStationInput.cpp
    apps/ground_station/GroundStationOptions.cpp
    apps/ground_station/GroundStationPipeline.cpp
    apps/ground_station/GroundStationProcessing.cpp
    apps/ground_station/GroundStationUsage.cpp
    apps/ground_station/GroundStationWorkers.cpp
    apps/ground_station/PosixSignalStopController.cpp
    apps/ground_station/TelemetryOutput.cpp)
target_link_libraries(stgs_ground_station PRIVATE stgs_network stgs_core Threads::Threads)
target_compile_options(stgs_ground_station PRIVATE ${STGS_WARNINGS})

add_executable(stgs_satellite_simulator
    apps/satellite_simulator.cpp
    apps/satellite_simulator/SimulatorApp.cpp
    apps/satellite_simulator/SimulatorFrameFactory.cpp
    apps/satellite_simulator/SimulatorOptions.cpp
    apps/satellite_simulator/SimulatorSocket.cpp
    apps/satellite_simulator/SimulatorUsage.cpp)
target_link_libraries(stgs_satellite_simulator PRIVATE stgs_network stgs_core Threads::Threads)
target_compile_options(stgs_satellite_simulator PRIVATE ${STGS_WARNINGS})

add_executable(stgs_port_check apps/port_check.cpp)
target_link_libraries(stgs_port_check PRIVATE stgs_network stgs_core Threads::Threads)
target_compile_options(stgs_port_check PRIVATE ${STGS_WARNINGS})

if(STGS_BUILD_TESTS)
    enable_testing()

    add_executable(stgs_tests
        tests/test_main.cpp
        tests/test_application_signal.cpp
        tests/test_codec.cpp
        tests/test_network.cpp
        tests/test_replay_health.cpp
        tests/support/TestSupport.cpp)
    target_link_libraries(stgs_tests PRIVATE stgs_network stgs_core Threads::Threads)
    target_compile_options(stgs_tests PRIVATE ${STGS_WARNINGS})

    add_test(NAME stgs_unit_and_integration_tests COMMAND stgs_tests)
endif()

if(STGS_BUILD_BENCHMARKS)
    add_executable(stgs_benchmark_decode benchmarks/benchmark_decode.cpp)
    target_link_libraries(stgs_benchmark_decode PRIVATE stgs_core Threads::Threads)
    target_compile_options(stgs_benchmark_decode PRIVATE ${STGS_WARNINGS})
endif()
