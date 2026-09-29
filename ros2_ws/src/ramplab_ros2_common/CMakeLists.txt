cmake_minimum_required(VERSION 3.20)
project(ramplab_ros2_common LANGUAGES CXX)

find_package(ament_cmake REQUIRED)

add_library(ramplab_ros2_coordinates INTERFACE)
target_include_directories(ramplab_ros2_coordinates INTERFACE
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<INSTALL_INTERFACE:include>")
target_compile_features(ramplab_ros2_coordinates INTERFACE cxx_std_20)

install(DIRECTORY include/ DESTINATION include)
install(TARGETS ramplab_ros2_coordinates EXPORT export_ramplab_ros2_coordinates)
ament_export_targets(export_ramplab_ros2_coordinates HAS_LIBRARY_TARGET)
ament_export_include_directories(include)
ament_package()
