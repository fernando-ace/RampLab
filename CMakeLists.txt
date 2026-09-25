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

add_library(airside_sim INTERFACE)
add_library(airside::sim ALIAS airside_sim)
target_include_directories(airside_sim INTERFACE include)

if(MSVC)
    target_compile_options(airside_sim INTERFACE /W4 /permissive- /EHsc)
else()
    target_compile_options(airside_sim INTERFACE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()
