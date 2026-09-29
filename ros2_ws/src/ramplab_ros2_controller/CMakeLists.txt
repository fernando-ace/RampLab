cmake_minimum_required(VERSION 3.20)
project(ramplab_ros2_controller LANGUAGES CXX)

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(sensor_msgs REQUIRED)
find_package(nav_msgs REQUIRED)
find_package(geometry_msgs REQUIRED)
find_package(ramplab_ros2_common REQUIRED)

add_executable(ramplab_ros2_controller src/controller_main.cpp)
target_compile_features(ramplab_ros2_controller PRIVATE cxx_std_20)
target_link_libraries(ramplab_ros2_controller PRIVATE
  rclcpp::rclcpp sensor_msgs::sensor_msgs nav_msgs::nav_msgs geometry_msgs::geometry_msgs
  ramplab_ros2_common::ramplab_ros2_coordinates)
install(TARGETS ramplab_ros2_controller DESTINATION lib/${PROJECT_NAME})
ament_package()
