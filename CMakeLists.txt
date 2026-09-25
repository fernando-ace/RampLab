cmake_minimum_required(VERSION 3.24)

project(
    airside_sim
    VERSION 0.2.0
    DESCRIPTION "Deterministic airport airside discrete-event simulation engine"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)
include(FetchContent)

# yaml-cpp 0.8.0 declares a pre-3.5 policy baseline; CMake 4 requires an explicit floor.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(YAML_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    yaml-cpp
    URL https://github.com/jbeder/yaml-cpp/archive/refs/tags/0.8.0.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    PATCH_COMMAND
        "${CMAKE_COMMAND}" -DYAML_CPP_SOURCE_DIR=<SOURCE_DIR>
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/patch-yaml-cpp.cmake"
)
FetchContent_MakeAvailable(yaml-cpp)

add_library(airside_sim STATIC
    src/core/event_queue.cpp
    src/core/event_stream.cpp
    src/integration/visualization.cpp
    src/world/airport_graph.cpp
    src/routing/astar.cpp
    src/agents/aircraft.cpp
    src/agents/service_vehicle.cpp
    src/operations/resource_pool.cpp
    src/operations/simulation.cpp
    src/metrics/metrics.cpp
)
add_library(airside::sim ALIAS airside_sim)
target_include_directories(airside_sim PUBLIC include)

function(airside_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /EHsc /Zc:__cplusplus)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    endif()
endfunction()

airside_enable_warnings(airside_sim)

add_library(airside_scenario STATIC src/scenario/scenario_loader.cpp)
add_library(airside::scenario ALIAS airside_scenario)
target_include_directories(airside_scenario PUBLIC include)
target_link_libraries(airside_scenario PUBLIC airside::sim PRIVATE yaml-cpp::yaml-cpp)
airside_enable_warnings(airside_scenario)

add_executable(airside_cli apps/airside_cli/main.cpp)
target_link_libraries(airside_cli PRIVATE airside::sim airside::scenario)

airside_enable_warnings(airside_cli)

if(BUILD_TESTING)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(googletest)

    add_executable(airside_tests
        tests/core/event_queue_test.cpp
        tests/core/event_stream_test.cpp
        tests/integration/snapshot_test.cpp
        tests/integration/visualization_test.cpp
        tests/scenario/scenario_loader_test.cpp
        tests/routing/astar_test.cpp
        tests/agents/state_machine_test.cpp
        tests/operations/resource_pool_test.cpp
        tests/operations/simulation_test.cpp
        tests/metrics/metrics_test.cpp
    )
    target_include_directories(airside_tests PRIVATE tests)
    target_compile_definitions(airside_tests PRIVATE AIRSIDE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(airside_tests PRIVATE airside::sim airside::scenario GTest::gtest_main)
    airside_enable_warnings(airside_tests)
    include(GoogleTest)
    gtest_discover_tests(airside_tests)
endif()
