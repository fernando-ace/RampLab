cmake_minimum_required(VERSION 3.24)

project(
    airside_sim
    VERSION 0.1.0
    DESCRIPTION "Deterministic airport airside discrete-event simulation engine"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

add_library(airside_sim STATIC
    src/core/event_queue.cpp
    src/core/event_stream.cpp
    src/world/airport_graph.cpp
    src/routing/astar.cpp
    src/agents/aircraft.cpp
    src/agents/service_vehicle.cpp
    src/operations/resource_pool.cpp
    src/operations/simulation.cpp
    src/metrics/metrics.cpp
    src/world/baseline_scenario.cpp
)
add_library(airside::sim ALIAS airside_sim)
target_include_directories(airside_sim PUBLIC include)

if(MSVC)
    target_compile_options(airside_sim PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(airside_sim PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

add_executable(airside_cli apps/airside_cli/main.cpp)
target_link_libraries(airside_cli PRIVATE airside::sim)

if(MSVC)
    target_compile_options(airside_cli PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(airside_cli PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

if(BUILD_TESTING)
    include(FetchContent)
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
        tests/routing/astar_test.cpp
        tests/agents/state_machine_test.cpp
        tests/operations/resource_pool_test.cpp
        tests/operations/simulation_test.cpp
        tests/metrics/metrics_test.cpp
    )
    target_link_libraries(airside_tests PRIVATE airside::sim GTest::gtest_main)
    include(GoogleTest)
    gtest_discover_tests(airside_tests)
endif()
