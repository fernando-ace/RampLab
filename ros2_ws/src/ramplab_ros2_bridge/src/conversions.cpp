#include "ramplab_ros2_bridge/conversions.hpp"

#include "ramplab_ros2_common/geodesy.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace ramplab_ros2_bridge {

builtin_interfaces::msg::Time to_ros_time(double seconds) {
  if (!std::isfinite(seconds) || seconds < 0.0 ||
      seconds > static_cast<double>(std::numeric_limits<std::int32_t>::max())) {
    throw std::invalid_argument("ROS simulation timestamp is outside the supported range");
  }
  const auto whole = static_cast<std::int64_t>(std::floor(seconds));
  auto nanos = static_cast<std::int64_t>(std::llround((seconds - static_cast<double>(whole)) * 1.0e9));
  auto normalized_whole = whole;
  if (nanos == 1'000'000'000) {
    ++normalized_whole;
    nanos = 0;
  }
  if (normalized_whole > std::numeric_limits<std::int32_t>::max()) {
    throw std::invalid_argument("ROS simulation timestamp is outside the supported range");
  }
  builtin_interfaces::msg::Time result;
  result.sec = static_cast<std::int32_t>(normalized_whole);
  result.nanosec = static_cast<std::uint32_t>(nanos);
  return result;
}

double from_ros_time(const builtin_interfaces::msg::Time& stamp) noexcept {
  return static_cast<double>(stamp.sec) + static_cast<double>(stamp.nanosec) / 1.0e9;
}

geometry_msgs::msg::Quaternion yaw_quaternion(double yaw_rad) {
  if (!std::isfinite(yaw_rad)) throw std::invalid_argument("yaw must be finite");
  geometry_msgs::msg::Quaternion result;
  result.z = std::sin(yaw_rad * 0.5);
  result.w = std::cos(yaw_rad * 0.5);
  return result;
}

sensor_msgs::msg::LaserScan to_laser_scan(
    const airside::autonomy::LidarScan& scan, double scan_period_s, const std::string& frame) {
  if (scan.ranges_m.empty() || !std::isfinite(scan_period_s) || scan_period_s <= 0.0) {
    throw std::invalid_argument("LiDAR scan and period must be valid");
  }
  sensor_msgs::msg::LaserScan result;
  result.header.stamp = to_ros_time(scan.timestamp_s);
  result.header.frame_id = frame;
  result.angle_min = static_cast<float>(scan.angle_min_rad);
  result.angle_increment = static_cast<float>(scan.angle_increment_rad);
  result.angle_max = static_cast<float>(scan.angle_min_rad + scan.angle_increment_rad *
      static_cast<double>(scan.ranges_m.size() - 1));
  result.time_increment = 0.0F;
  result.scan_time = static_cast<float>(scan_period_s);
  result.range_min = static_cast<float>(scan.range_min_m);
  result.range_max = static_cast<float>(scan.range_max_m);
  result.ranges.reserve(scan.ranges_m.size());
  for (double range : scan.ranges_m) result.ranges.push_back(static_cast<float>(range));
  return result;
}

sensor_msgs::msg::Imu to_imu(
    const airside::autonomy::ImuMeasurement& measurement,
    const airside::autonomy::SensorConfig& config, const std::string& frame) {
  sensor_msgs::msg::Imu result;
  result.header.stamp = to_ros_time(measurement.timestamp_s);
  result.header.frame_id = frame;
  result.orientation = yaw_quaternion(measurement.heading_rad);
  // The model constrains roll and pitch to zero and simulates only yaw.
  result.orientation_covariance[8] = config.imu_heading_sigma_rad * config.imu_heading_sigma_rad;
  result.angular_velocity.z = measurement.yaw_rate_radps;
  result.angular_velocity_covariance[8] =
      config.imu_yaw_rate_sigma_radps * config.imu_yaw_rate_sigma_radps;
  result.linear_acceleration.x = measurement.longitudinal_accel_mps2;
  result.linear_acceleration_covariance[0] =
      config.imu_accel_sigma_mps2 * config.imu_accel_sigma_mps2;
  return result;
}

sensor_msgs::msg::NavSatFix to_nav_sat_fix(
    const airside::autonomy::GnssMeasurement& measurement, const std::string& frame) {
  const auto fix = ramplab_ros2_common::local_to_geodetic(
      {measurement.position.x_m, measurement.position.y_m, 0.0});
  sensor_msgs::msg::NavSatFix result;
  result.header.stamp = to_ros_time(measurement.timestamp_s);
  result.header.frame_id = frame;
  result.status.status = sensor_msgs::msg::NavSatStatus::STATUS_FIX;
  result.status.service = sensor_msgs::msg::NavSatStatus::SERVICE_GPS;
  result.latitude = fix.latitude_deg;
  result.longitude = fix.longitude_deg;
  result.altitude = fix.altitude_m;
  const double sigma = measurement.accuracy_m / 1.96;
  const double variance = sigma * sigma;
  result.position_covariance[0] = variance;
  result.position_covariance[4] = variance;
  result.position_covariance_type = sensor_msgs::msg::NavSatFix::COVARIANCE_TYPE_DIAGONAL_KNOWN;
  return result;
}

nav_msgs::msg::Odometry to_odometry(
    double timestamp_s, airside::Vec2 position, double heading_rad,
    double speed_mps, double yaw_rate_radps) {
  nav_msgs::msg::Odometry result;
  result.header.stamp = to_ros_time(timestamp_s);
  result.header.frame_id = "odom";
  result.child_frame_id = "base_link";
  result.pose.pose.position.x = position.x_m;
  result.pose.pose.position.y = position.y_m;
  result.pose.pose.orientation = yaw_quaternion(heading_rad);
  result.twist.twist.linear.x = speed_mps;
  result.twist.twist.angular.z = yaw_rate_radps;
  return result;
}

std::optional<airside::autonomy::VehicleCommand> from_twist(
    const geometry_msgs::msg::Twist& message,
    const airside::autonomy::VehicleLimits& limits) noexcept {
  const double values[] = {message.linear.x, message.linear.y, message.linear.z,
      message.angular.x, message.angular.y, message.angular.z};
  for (double value : values) if (!std::isfinite(value)) return std::nullopt;
  return airside::autonomy::VehicleCommand{
      std::clamp(message.linear.x, 0.0, limits.maximum_speed_mps),
      std::clamp(message.angular.z, -limits.maximum_yaw_rate_radps, limits.maximum_yaw_rate_radps)};
}

}  // namespace ramplab_ros2_bridge
