#include "ramplab_ros2_common/geodesy.hpp"

#include <geometry_msgs/msg/twist.hpp>
#include <nav_msgs/msg/odometry.hpp>
#include <nav_msgs/msg/path.hpp>
#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/imu.hpp>
#include <sensor_msgs/msg/laser_scan.hpp>
#include <sensor_msgs/msg/nav_sat_fix.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <vector>

namespace {
struct Point { double x{}; double y{}; };
double distance(Point a, Point b) { return std::hypot(a.x - b.x, a.y - b.y); }
double wrap_angle(double angle) { return std::remainder(angle, 2.0 * std::numbers::pi); }

class RouteController final : public rclcpp::Node {
public:
  RouteController() : Node("ramplab_external_controller", "/ramplab/tug1") {
    maximum_speed_mps_ = declare_parameter<double>("maximum_speed_mps", 5.0);
    maximum_deceleration_mps2_ = declare_parameter<double>("maximum_deceleration_mps2", 1.5);
    maximum_yaw_rate_radps_ = declare_parameter<double>("maximum_yaw_rate_radps", 0.8);
    safety_stop_range_m_ = declare_parameter<double>("safety_stop_range_m", 2.2);
    const auto sensor_qos = rclcpp::SensorDataQoS().keep_last(1);
    const auto command_qos = rclcpp::QoS(rclcpp::KeepLast(1)).best_effort().durability_volatile();
    command_pub_ = create_publisher<geometry_msgs::msg::Twist>("cmd_vel", command_qos);
    route_sub_ = create_subscription<nav_msgs::msg::Path>("route",
        rclcpp::QoS(rclcpp::KeepLast(1)).reliable().transient_local(),
        [this](nav_msgs::msg::Path::ConstSharedPtr message) { receive_route(*message); });
    imu_sub_ = create_subscription<sensor_msgs::msg::Imu>("imu", sensor_qos,
        [this](sensor_msgs::msg::Imu::ConstSharedPtr message) { imu_ = *message; });
    odom_sub_ = create_subscription<nav_msgs::msg::Odometry>("odom", sensor_qos,
        [this](nav_msgs::msg::Odometry::ConstSharedPtr message) { update_from_odometry(*message); });
    gnss_sub_ = create_subscription<sensor_msgs::msg::NavSatFix>("gnss", sensor_qos,
        [this](sensor_msgs::msg::NavSatFix::ConstSharedPtr message) { receive_gnss(*message); });
    scan_sub_ = create_subscription<sensor_msgs::msg::LaserScan>("scan", sensor_qos,
        [this](sensor_msgs::msg::LaserScan::ConstSharedPtr message) { scan_ = *message; });
    RCLCPP_INFO(get_logger(), "External route controller started; it consumes ROS messages only");
  }

  void report() const {
    RCLCPP_INFO(get_logger(), "Controller stopped: commands=%zu, LiDAR emergency stops=%zu, "
        "goal stop latched=%s", commands_published_, emergency_stops_,
        goal_stop_latched_ ? "true" : "false");
  }

private:
  void receive_route(const nav_msgs::msg::Path& message) {
    if (message.header.frame_id != "map" || message.poses.size() < 2) {
      RCLCPP_ERROR(get_logger(), "Rejected route: expected at least two waypoints in map frame");
      waypoints_.clear();
      return;
    }
    waypoints_.clear();
    waypoints_.reserve(message.poses.size());
    for (const auto& pose : message.poses) {
      waypoints_.push_back({pose.pose.position.x, pose.pose.position.y});
    }
    target_waypoint_index_ = std::min<std::size_t>(1, waypoints_.size() - 1);
    RCLCPP_INFO(get_logger(), "Received %zu route waypoints from ROS 2", waypoints_.size());
  }

  void receive_gnss(const sensor_msgs::msg::NavSatFix& message) {
    if (message.status.status < sensor_msgs::msg::NavSatStatus::STATUS_FIX ||
        !std::isfinite(message.latitude) || !std::isfinite(message.longitude)) {
      gnss_.reset();
      return;
    }
    gnss_ = ramplab_ros2_common::geodetic_to_local(
        {message.latitude, message.longitude, message.altitude});
    gnss_stamp_ = static_cast<double>(message.header.stamp.sec) +
        static_cast<double>(message.header.stamp.nanosec) / 1.0e9;
  }

  void update_from_odometry(const nav_msgs::msg::Odometry& message) {
    if (message.header.frame_id != "odom" || message.child_frame_id != "base_link") return;
    const Point odom_point{message.pose.pose.position.x, message.pose.pose.position.y};
    if (!have_odom_) {
      estimated_position_ = odom_point;
      have_odom_ = true;
    } else {
      estimated_position_.x += odom_point.x - last_odom_position_.x;
      estimated_position_.y += odom_point.y - last_odom_position_.y;
    }
    last_odom_position_ = odom_point;
    if (gnss_ && gnss_stamp_ != last_fused_gnss_stamp_) {
      estimated_position_.x = 0.85 * estimated_position_.x + 0.15 * gnss_->east_m;
      estimated_position_.y = 0.85 * estimated_position_.y + 0.15 * gnss_->north_m;
      last_fused_gnss_stamp_ = gnss_stamp_;
    }
    if (waypoints_.size() < 2 || !imu_ || !scan_ || !gnss_) {
      publish_command({});
      return;
    }
    const double heading = 2.0 * std::atan2(imu_->orientation.z, imu_->orientation.w);
    const Point goal = waypoints_.back();
    const double goal_distance = distance(estimated_position_, goal);
    if (goal_distance <= 1.5) goal_stop_latched_ = true;
    if (goal_stop_latched_) {
      publish_command({});
      return;
    }
    if (blocked_ahead(*scan_)) {
      if (!safety_active_) {
        ++emergency_stops_;
        RCLCPP_WARN(get_logger(), "LiDAR safety stop %zu: forward return inside %.2f m",
                    emergency_stops_, safety_stop_range_m_);
      }
      safety_active_ = true;
      publish_command({});
      return;
    }
    safety_active_ = false;
    target_waypoint_index_ = std::min(target_waypoint_index_, waypoints_.size() - 1);
    while (target_waypoint_index_ + 1 < waypoints_.size() &&
        distance(estimated_position_, waypoints_[target_waypoint_index_]) <= 8.0) {
      ++target_waypoint_index_;
    }
    const auto target = waypoints_[target_waypoint_index_];
    const double desired_heading = std::atan2(target.y - estimated_position_.y,
        target.x - estimated_position_.x);
    const double heading_error = wrap_angle(desired_heading - heading);
    const double yaw_rate = std::clamp(1.8 * heading_error,
        -maximum_yaw_rate_radps_, maximum_yaw_rate_radps_);
    const double turn_speed = std::max(1.0, maximum_speed_mps_ *
        (1.0 - std::min(0.8, std::abs(heading_error) / std::numbers::pi)));
    const double stopping_speed = std::sqrt(std::max(0.0,
        2.0 * maximum_deceleration_mps2_ * (goal_distance - 0.25)));
    publish_command({std::min({maximum_speed_mps_, turn_speed, stopping_speed}), yaw_rate});
  }

  bool blocked_ahead(const sensor_msgs::msg::LaserScan& scan) const {
    for (std::size_t i = 0; i < scan.ranges.size(); ++i) {
      const double angle = static_cast<double>(scan.angle_min) +
          static_cast<double>(i) * static_cast<double>(scan.angle_increment);
      const double range = scan.ranges[i];
      if (std::isnan(range)) return true;
      if (std::abs(angle) <= 0.20 && std::isfinite(range) && range < safety_stop_range_m_) return true;
    }
    return false;
  }

  void publish_command(std::pair<double, double> command) {
    geometry_msgs::msg::Twist message;
    message.linear.x = std::clamp(command.first, 0.0, maximum_speed_mps_);
    message.angular.z = std::clamp(command.second, -maximum_yaw_rate_radps_, maximum_yaw_rate_radps_);
    command_pub_->publish(message);
    ++commands_published_;
    if (commands_published_ == 1 || commands_published_ % 100 == 0) {
      RCLCPP_INFO(get_logger(), "cmd_vel #%zu: linear.x=%.3f angular.z=%.3f; lidar stops=%zu",
          commands_published_, message.linear.x, message.angular.z, emergency_stops_);
    }
  }

  double maximum_speed_mps_{5.0};
  double maximum_deceleration_mps2_{1.5};
  double maximum_yaw_rate_radps_{0.8};
  double safety_stop_range_m_{2.2};
  rclcpp::Publisher<geometry_msgs::msg::Twist>::SharedPtr command_pub_;
  rclcpp::Subscription<nav_msgs::msg::Path>::SharedPtr route_sub_;
  rclcpp::Subscription<sensor_msgs::msg::Imu>::SharedPtr imu_sub_;
  rclcpp::Subscription<nav_msgs::msg::Odometry>::SharedPtr odom_sub_;
  rclcpp::Subscription<sensor_msgs::msg::NavSatFix>::SharedPtr gnss_sub_;
  rclcpp::Subscription<sensor_msgs::msg::LaserScan>::SharedPtr scan_sub_;
  std::optional<sensor_msgs::msg::Imu> imu_;
  std::optional<sensor_msgs::msg::LaserScan> scan_;
  std::optional<ramplab_ros2_common::LocalPoint> gnss_;
  double gnss_stamp_{-1.0};
  double last_fused_gnss_stamp_{-1.0};
  std::vector<Point> waypoints_;
  Point estimated_position_{};
  Point last_odom_position_{};
  std::size_t target_waypoint_index_{1};
  std::size_t emergency_stops_{}, commands_published_{};
  bool have_odom_{}, safety_active_{};
  bool goal_stop_latched_{};
};
}  // namespace

int main(int argc, char** argv) {
  rclcpp::init(argc, argv);
  auto node = std::make_shared<RouteController>();
  rclcpp::spin(node);
  node->report();
  rclcpp::shutdown();
  return 0;
}
