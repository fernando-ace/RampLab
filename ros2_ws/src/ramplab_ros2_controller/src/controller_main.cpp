#include <geometry_msgs/msg/twist.hpp>
#include <builtin_interfaces/msg/time.hpp>
#include <nav_msgs/msg/odometry.hpp>
#include <nav_msgs/msg/path.hpp>
#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/laser_scan.hpp>
#include <std_msgs/msg/u_int8.hpp>

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
    localization_timeout_s_ = declare_parameter<double>("localization_timeout_s", 3.0);
    perception_timeout_s_ = declare_parameter<double>("perception_timeout_s", 0.5);
    const auto sensor_qos = rclcpp::SensorDataQoS().keep_last(1);
    const auto command_qos = rclcpp::QoS(rclcpp::KeepLast(1)).best_effort().durability_volatile();
    command_pub_ = create_publisher<geometry_msgs::msg::Twist>("cmd_vel", command_qos);
    route_sub_ = create_subscription<nav_msgs::msg::Path>("route",
        rclcpp::QoS(rclcpp::KeepLast(1)).reliable().transient_local(),
        [this](nav_msgs::msg::Path::ConstSharedPtr message) { receive_route(*message); });
    filtered_odom_sub_ = create_subscription<nav_msgs::msg::Odometry>("filtered_odom", sensor_qos,
        [this](nav_msgs::msg::Odometry::ConstSharedPtr message) { update_from_filtered_odometry(*message); });
    health_sub_ = create_subscription<std_msgs::msg::UInt8>("estimator_health", sensor_qos,
        [this](std_msgs::msg::UInt8::ConstSharedPtr message) { estimator_health_ = message->data; });
    scan_sub_ = create_subscription<sensor_msgs::msg::LaserScan>("scan", sensor_qos,
        [this](sensor_msgs::msg::LaserScan::ConstSharedPtr message) { scan_ = *message; });
    RCLCPP_INFO(get_logger(), "External route controller started; it consumes ROS messages only");
  }

  void report() const {
    RCLCPP_INFO(get_logger(), "Controller stopped: commands=%zu, LiDAR emergency stops=%zu, "
        "degraded safety stops=%zu, goal stop latched=%s", commands_published_, emergency_stops_, degraded_safety_stops_,
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

  void update_from_filtered_odometry(const nav_msgs::msg::Odometry& message) {
    if (message.header.frame_id != "odom" || message.child_frame_id != "base_link") return;
    if (!std::isfinite(message.pose.pose.position.x) || !std::isfinite(message.pose.pose.position.y)) return;
    estimated_position_ = {message.pose.pose.position.x, message.pose.pose.position.y};
    estimated_heading_ = 2.0 * std::atan2(message.pose.pose.orientation.z, message.pose.pose.orientation.w);
    const double clock_now = get_clock()->now().seconds();
    const double now = std::isfinite(clock_now) && clock_now > 0.0 ? clock_now : stamp_seconds(message.header.stamp);
    const bool perception_stale = !scan_ || now - stamp_seconds(scan_->header.stamp) > perception_timeout_s_;
    const bool estimate_stale = now - stamp_seconds(message.header.stamp) > localization_timeout_s_;
    const bool estimator_unsafe = !estimator_health_ || *estimator_health_ == 0 || *estimator_health_ >= 3;
    if (waypoints_.size() < 2 || estimate_stale || perception_stale || estimator_unsafe) {
      if ((estimate_stale || perception_stale || estimator_unsafe) && !degraded_stop_active_) {
        ++degraded_safety_stops_;
        RCLCPP_WARN(get_logger(), "Estimator or perception health is unsafe/stale; commanding stop");
      }
      degraded_stop_active_ = estimate_stale || perception_stale || estimator_unsafe;
      publish_command({});
      return;
    }
    degraded_stop_active_ = false;
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
    const double heading_error = wrap_angle(desired_heading - estimated_heading_);
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

  static double stamp_seconds(const builtin_interfaces::msg::Time& stamp) {
    return static_cast<double>(stamp.sec) + static_cast<double>(stamp.nanosec) / 1.0e9;
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
  double localization_timeout_s_{3.0};
  double perception_timeout_s_{0.5};
  rclcpp::Publisher<geometry_msgs::msg::Twist>::SharedPtr command_pub_;
  rclcpp::Subscription<nav_msgs::msg::Path>::SharedPtr route_sub_;
  rclcpp::Subscription<nav_msgs::msg::Odometry>::SharedPtr filtered_odom_sub_;
  rclcpp::Subscription<std_msgs::msg::UInt8>::SharedPtr health_sub_;
  rclcpp::Subscription<sensor_msgs::msg::LaserScan>::SharedPtr scan_sub_;
  std::optional<sensor_msgs::msg::LaserScan> scan_;
  std::vector<Point> waypoints_;
  Point estimated_position_{};
  double estimated_heading_{};
  std::size_t target_waypoint_index_{1};
  std::size_t emergency_stops_{}, commands_published_{};
  std::size_t degraded_safety_stops_{};
  std::optional<std::uint8_t> estimator_health_;
  bool safety_active_{}, degraded_stop_active_{};
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
