#pragma once

#include "airside/autonomy/simulation.hpp"

#include <builtin_interfaces/msg/time.hpp>
#include <geometry_msgs/msg/twist.hpp>
#include <nav_msgs/msg/odometry.hpp>
#include <sensor_msgs/msg/imu.hpp>
#include <sensor_msgs/msg/laser_scan.hpp>
#include <sensor_msgs/msg/nav_sat_fix.hpp>

#include <optional>
#include <regex>
#include <string>

namespace ramplab_ros2_bridge {

[[nodiscard]] inline std::string vehicle_namespace(const std::string& id) {
  if(id.empty()||!std::regex_match(id,std::regex("[A-Za-z0-9_]+")))throw std::invalid_argument("vehicle ID must contain only letters, digits, or underscore");
  return "/ramplab/"+id;
}

[[nodiscard]] builtin_interfaces::msg::Time to_ros_time(double seconds);
[[nodiscard]] double from_ros_time(const builtin_interfaces::msg::Time& stamp) noexcept;
[[nodiscard]] geometry_msgs::msg::Quaternion yaw_quaternion(double yaw_rad);
[[nodiscard]] sensor_msgs::msg::LaserScan to_laser_scan(
    const airside::autonomy::LidarScan& scan, double scan_period_s, const std::string& frame);
[[nodiscard]] sensor_msgs::msg::Imu to_imu(
    const airside::autonomy::ImuMeasurement& measurement,
    const airside::autonomy::SensorConfig& config, const std::string& frame);
[[nodiscard]] sensor_msgs::msg::NavSatFix to_nav_sat_fix(
    const airside::autonomy::GnssMeasurement& measurement, const std::string& frame);
[[nodiscard]] nav_msgs::msg::Odometry to_odometry(
    double timestamp_s, airside::Vec2 position, double heading_rad,
    double speed_mps, double yaw_rate_radps);
[[nodiscard]] nav_msgs::msg::Odometry to_filtered_odometry(
    const airside::autonomy::EstimatedState& estimate);
[[nodiscard]] std::optional<airside::autonomy::VehicleCommand> from_twist(
    const geometry_msgs::msg::Twist& message,
    const airside::autonomy::VehicleLimits& limits) noexcept;

}  // namespace ramplab_ros2_bridge
