cmake_minimum_required(VERSION 3.20)
project(ramplab_ros2_bridge LANGUAGES CXX)

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(sensor_msgs REQUIRED)
find_package(nav_msgs REQUIRED)
find_package(geometry_msgs REQUIRED)
find_package(rosgraph_msgs REQUIRED)
find_package(tf2_ros REQUIRED)
find_package(std_msgs REQUIRED)
find_package(ramplab_ros2_common REQUIRED)

set(RAMPLAB_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../.." CACHE PATH
  "RampLab repository root")
get_filename_component(RAMPLAB_SOURCE_DIR "${RAMPLAB_SOURCE_DIR}" ABSOLUTE)
set(RAMPLAB_BUILD_DIR "${RAMPLAB_SOURCE_DIR}/build-final-msvc" CACHE PATH
  "Release CMake build containing RampLab static libraries")

set(_ramplab_release "${RAMPLAB_BUILD_DIR}/Release")
set(_ramplab_yaml "${RAMPLAB_BUILD_DIR}/_deps/yaml-cpp-build/Release/yaml-cpp.lib")
foreach(_library airside_sim airside_autonomy airside_scenario airside_autonomy_scenario)
  if(NOT EXISTS "${_ramplab_release}/${_library}.lib")
    message(FATAL_ERROR "Missing ${_library}.lib; build RampLab Release first (${_ramplab_release})")
  endif()
  add_library(ramplab_${_library} STATIC IMPORTED GLOBAL)
  set_target_properties(ramplab_${_library} PROPERTIES
    IMPORTED_LOCATION "${_ramplab_release}/${_library}.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${RAMPLAB_SOURCE_DIR}/include")
endforeach()
if(NOT EXISTS "${_ramplab_yaml}")
  message(FATAL_ERROR "Missing yaml-cpp.lib; build the normal RampLab Release first")
endif()
add_library(ramplab_yaml_cpp STATIC IMPORTED GLOBAL)
set_target_properties(ramplab_yaml_cpp PROPERTIES
  IMPORTED_LOCATION "${_ramplab_yaml}"
  INTERFACE_INCLUDE_DIRECTORIES "${RAMPLAB_BUILD_DIR}/_deps/yaml-cpp-src/include")

add_library(ramplab_ros2_conversions STATIC src/conversions.cpp src/camera_transport.cpp)
target_compile_features(ramplab_ros2_conversions PUBLIC cxx_std_20)
target_include_directories(ramplab_ros2_conversions PUBLIC include)
target_link_libraries(ramplab_ros2_conversions PUBLIC
  ramplab_airside_autonomy
  sensor_msgs::sensor_msgs nav_msgs::nav_msgs geometry_msgs::geometry_msgs
  std_msgs::std_msgs
  builtin_interfaces::builtin_interfaces ramplab_ros2_common::ramplab_ros2_coordinates)
if(WIN32)
  target_link_libraries(ramplab_ros2_conversions PUBLIC ws2_32)
endif()

add_executable(ramplab_ros2_bridge src/bridge_main.cpp)
target_compile_features(ramplab_ros2_bridge PRIVATE cxx_std_20)
target_link_libraries(ramplab_ros2_bridge PRIVATE
  rclcpp::rclcpp sensor_msgs::sensor_msgs nav_msgs::nav_msgs geometry_msgs::geometry_msgs
  rosgraph_msgs::rosgraph_msgs std_msgs::std_msgs tf2_ros::tf2_ros ramplab_ros2_conversions
  ramplab_airside_autonomy_scenario ramplab_airside_autonomy
  ramplab_airside_scenario ramplab_airside_sim ramplab_yaml_cpp)

install(TARGETS ramplab_ros2_bridge DESTINATION lib/${PROJECT_NAME})

add_executable(ramplab_ros2_fleet_bridge src/fleet_bridge_main.cpp)
target_compile_features(ramplab_ros2_fleet_bridge PRIVATE cxx_std_20)
target_link_libraries(ramplab_ros2_fleet_bridge PRIVATE
  rclcpp::rclcpp std_msgs::std_msgs
  ramplab_airside_autonomy_scenario ramplab_airside_autonomy
  ramplab_airside_scenario ramplab_airside_sim ramplab_yaml_cpp)
install(TARGETS ramplab_ros2_fleet_bridge DESTINATION lib/${PROJECT_NAME})

if(BUILD_TESTING)
  find_package(ament_cmake_gtest REQUIRED)
  ament_add_gtest(test_ros2_conversions test/test_conversions.cpp)
  if(TARGET test_ros2_conversions)
    target_compile_features(test_ros2_conversions PRIVATE cxx_std_20)
    target_include_directories(test_ros2_conversions PRIVATE include)
    target_link_libraries(test_ros2_conversions ramplab_ros2_conversions)
  endif()
endif()

ament_package()
