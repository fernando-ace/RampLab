#include "airside/autonomy/scenario_loader.hpp"
#include "airside/autonomy/simulation.hpp"
#include "ramplab_ros2_bridge/command_watchdog.hpp"
#include "ramplab_ros2_bridge/conversions.hpp"

#include <builtin_interfaces/msg/time.hpp>
#include <geometry_msgs/msg/transform_stamped.hpp>
#include <nav_msgs/msg/path.hpp>
#include <rclcpp/rclcpp.hpp>
#include <rosgraph_msgs/msg/clock.hpp>
#include <sensor_msgs/msg/imu.hpp>
#include <sensor_msgs/msg/laser_scan.hpp>
#include <sensor_msgs/msg/nav_sat_fix.hpp>
#include <tf2_ros/static_transform_broadcaster.hpp>
#include <tf2_ros/transform_broadcaster.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <iomanip>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
using airside::autonomy::AutonomySimulation;
using airside::autonomy::AutonomyScenario;
using airside::autonomy::IAutonomyController;
using airside::autonomy::MissionState;
using airside::autonomy::SensorFrame;
using airside::autonomy::VehicleCommand;
using airside::autonomy::VehicleLimits;
using ramplab_ros2_bridge::CommandWatchdog;

struct Options {
  std::string scenario{"scenarios/autonomy_tug.yaml"};
  std::optional<std::uint64_t> seed;
  double realtime_factor{1.0};
  double command_timeout_s{0.5};
  std::optional<double> max_simulation_s;
};

double parse_double(std::string_view text, std::string_view name) {
  std::size_t consumed{};
  const double value = std::stod(std::string(text), &consumed);
  if (consumed != text.size() || !std::isfinite(value)) {
    throw std::invalid_argument("invalid value for " + std::string(name));
  }
  return value;
}

Options parse_options(int argc, char** argv) {
  Options result;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg{argv[i]};
    const auto next = [&]() -> std::string_view {
      if (++i >= argc) throw std::invalid_argument("missing value after " + std::string(arg));
      return argv[i];
    };
    if (arg == "--scenario") result.scenario = next();
    else if (arg == "--seed") result.seed = std::stoull(std::string(next()));
    else if (arg == "--realtime-factor") result.realtime_factor = parse_double(next(), arg);
    else if (arg == "--command-timeout-s") result.command_timeout_s = parse_double(next(), arg);
    else if (arg == "--max-sim-seconds") result.max_simulation_s = parse_double(next(), arg);
    else if (arg == "--help") {
      std::cout << "ramplab_ros2_bridge [--scenario FILE] [--seed N] [--realtime-factor N] "
                   "[--command-timeout-s N] [--max-sim-seconds N]\n";
      std::exit(0);
    } else if (arg != "--ros-args" && arg != "--") {
      throw std::invalid_argument("unknown option: " + std::string(arg));
    }
  }
  if (result.realtime_factor <= 0.0 || result.realtime_factor > 20.0 ||
      result.command_timeout_s <= 0.0 ||
      (result.max_simulation_s && *result.max_simulation_s <= 0.0)) {
    throw std::invalid_argument("pacing factor, timeout, and run limit must be positive; "
                                "pacing factor may not exceed 20");
  }
  return result;
}

class ExternalCommand final : public IAutonomyController {
public:
  VehicleCommand update(const SensorFrame&, const MissionState&) override { return command_; }
  void set(VehicleCommand command) noexcept { command_ = command; }
private:
  VehicleCommand command_{};
};

class BridgeNode final : public rclcpp::Node {
public:
  BridgeNode(AutonomySimulation& simulation, const AutonomyScenario& scenario,
             double timeout_s)
      : Node("ramplab_bridge", "/ramplab/tug1"), simulation_(simulation), scenario_(scenario),
        timeout_s_(timeout_s), watchdog_(timeout_s, simulation.time_s()),
        scan_pub_(create_publisher<sensor_msgs::msg::LaserScan>("scan", rclcpp::SensorDataQoS().keep_last(1))),
        imu_pub_(create_publisher<sensor_msgs::msg::Imu>("imu", rclcpp::SensorDataQoS().keep_last(1))),
        odom_pub_(create_publisher<nav_msgs::msg::Odometry>("odom", rclcpp::SensorDataQoS().keep_last(1))),
        gnss_pub_(create_publisher<sensor_msgs::msg::NavSatFix>("gnss", rclcpp::SensorDataQoS().keep_last(1))),
        route_pub_(create_publisher<nav_msgs::msg::Path>("route",
            rclcpp::QoS(rclcpp::KeepLast(1)).reliable().transient_local())),
        clock_pub_(create_publisher<rosgraph_msgs::msg::Clock>("/clock",
            rclcpp::QoS(rclcpp::KeepLast(10)).reliable())) {
    cmd_sub_ = create_subscription<geometry_msgs::msg::Twist>("cmd_vel",
        rclcpp::QoS(rclcpp::KeepLast(1)).best_effort().durability_volatile(),
        [this](geometry_msgs::msg::Twist::ConstSharedPtr message) { receive_command(*message); });
  }

  void initialize_transforms() {
    const auto node = shared_from_this();
    const tf2_ros::StaticTransformBroadcaster::RequiredInterfaces static_interfaces{
        node->get_node_parameters_interface(), node->get_node_topics_interface()};
    const tf2_ros::TransformBroadcaster::RequiredInterfaces dynamic_interfaces{
        node->get_node_parameters_interface(), node->get_node_topics_interface()};
    static_tf_ = std::make_shared<tf2_ros::StaticTransformBroadcaster>(static_interfaces);
    dynamic_tf_ = std::make_shared<tf2_ros::TransformBroadcaster>(dynamic_interfaces);
    std::vector<geometry_msgs::msg::TransformStamped> transforms;
    transforms.push_back(identity_transform("map", "odom"));
    transforms.push_back(identity_transform("base_link", "lidar"));
    transforms.push_back(identity_transform("base_link", "imu"));
    static_tf_->sendTransform(transforms);
  }

  void publish_initial() {
    publish_route();
    publish_clock();
    publish_frame(simulation_.observe());
  }

  void spin_once() { rclcpp::spin_some(shared_from_this()); }

  void advance_one(ExternalCommand& adapter) {
    adapter.set(command_for_step());
    (void)simulation_.advance(adapter);
    publish_clock();
    publish_frame(simulation_.observe());
  }

  [[nodiscard]] std::size_t timeout_count() const noexcept { return watchdog_.timeout_count(); }
  [[nodiscard]] double latest_command_age() const noexcept {
    return watchdog_.age(simulation_.time_s());
  }
  [[nodiscard]] std::uint64_t count_scan() const noexcept { return scan_count_; }
  [[nodiscard]] std::uint64_t count_imu() const noexcept { return imu_count_; }
  [[nodiscard]] std::uint64_t count_odom() const noexcept { return odom_count_; }
  [[nodiscard]] std::uint64_t count_gnss() const noexcept { return gnss_count_; }

private:
  static geometry_msgs::msg::TransformStamped identity_transform(
      const std::string& parent, const std::string& child) {
    geometry_msgs::msg::TransformStamped tf;
    tf.header.frame_id = parent;
    tf.child_frame_id = child;
    tf.transform.rotation.w = 1.0;
    return tf;
  }

  void receive_command(const geometry_msgs::msg::Twist& message) {
    const auto command = ramplab_ros2_bridge::from_twist(message, scenario_.limits);
    if (!command) {
      RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 2000,
          "Rejected cmd_vel containing NaN or infinity; simulation-time timeout remains active");
      return;
    }
    const auto clamped = command->target_speed_mps != message.linear.x ||
        command->target_yaw_rate_radps != message.angular.z;
    if (clamped) {
      RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 2000,
          "Clamped cmd_vel to vehicle speed/yaw-rate limits");
    }
    if (watchdog_.accept(*command, simulation_.time_s())) {
      RCLCPP_INFO(get_logger(), "Fresh cmd_vel received; safe-stop latch cleared");
    }
  }

  VehicleCommand command_for_step() {
    const auto decision = watchdog_.poll(simulation_.time_s());
    if (decision.timeout_activated) {
      RCLCPP_WARN(get_logger(), "cmd_vel timeout at sim_time=%.3f s (age %.3f s, limit %.3f s); "
                  "requesting zero speed and yaw rate", simulation_.time_s(), decision.age_s, timeout_s_);
    }
    return decision.command;
  }

  void publish_clock() {
    rosgraph_msgs::msg::Clock message;
    message.clock = ramplab_ros2_bridge::to_ros_time(simulation_.time_s());
    clock_pub_->publish(message);
  }

  void publish_route() {
    nav_msgs::msg::Path route;
    route.header.stamp = ramplab_ros2_bridge::to_ros_time(simulation_.time_s());
    route.header.frame_id = "map";
    const auto& points = simulation_.mission().waypoints;
    route.poses.reserve(points.size());
    for (std::size_t i = 0; i < points.size(); ++i) {
      geometry_msgs::msg::PoseStamped pose;
      pose.header = route.header;
      pose.pose.position.x = points[i].x_m;
      pose.pose.position.y = points[i].y_m;
      if (i + 1 < points.size()) {
        pose.pose.orientation = ramplab_ros2_bridge::yaw_quaternion(std::atan2(
            points[i + 1].y_m - points[i].y_m, points[i + 1].x_m - points[i].x_m));
      } else if (i > 0) {
        pose.pose.orientation = ramplab_ros2_bridge::yaw_quaternion(std::atan2(
            points[i].y_m - points[i - 1].y_m, points[i].x_m - points[i - 1].x_m));
      } else {
        pose.pose.orientation.w = 1.0;
      }
      route.poses.push_back(std::move(pose));
    }
    route_pub_->publish(route);
  }

  void publish_frame(const SensorFrame& frame) {
    if (frame.lidar && frame.lidar->timestamp_s != last_scan_stamp_) {
      scan_pub_->publish(ramplab_ros2_bridge::to_laser_scan(
          *frame.lidar, 1.0 / scenario_.sensors.lidar_hz, "lidar"));
      last_scan_stamp_ = frame.lidar->timestamp_s;
      ++scan_count_;
    }
    if (frame.imu && frame.imu->timestamp_s != last_imu_stamp_) {
      imu_pub_->publish(ramplab_ros2_bridge::to_imu(*frame.imu, scenario_.sensors, "imu"));
      last_imu_stamp_ = frame.imu->timestamp_s;
      ++imu_count_;
    }
    if (frame.gnss && frame.gnss->timestamp_s != last_gnss_stamp_) {
      gnss_pub_->publish(ramplab_ros2_bridge::to_nav_sat_fix(*frame.gnss, "gnss"));
      last_gnss_stamp_ = frame.gnss->timestamp_s;
      ++gnss_count_;
    }
    if (frame.odometry && frame.odometry->timestamp_s != last_odom_stamp_) {
      update_odometry(*frame.odometry, frame.imu ? &*frame.imu : nullptr);
      auto message = ramplab_ros2_bridge::to_odometry(frame.odometry->timestamp_s,
          estimated_position_, estimated_heading_rad_, frame.odometry->speed_mps, estimated_yaw_rate_);
      odom_pub_->publish(message);
      publish_dynamic_transform(message);
      last_odom_stamp_ = frame.odometry->timestamp_s;
      ++odom_count_;
    }
  }

  void update_odometry(const airside::autonomy::OdometryMeasurement& measurement,
                       const airside::autonomy::ImuMeasurement* imu) {
    if (!odom_initialized_) {
      estimated_position_ = simulation_.mission().waypoints.front();
      estimated_heading_rad_ = imu ? imu->heading_rad : 0.0;
      last_measured_distance_m_ = measurement.distance_m;
      last_heading_change_rad_ = measurement.heading_change_rad;
      last_odom_stamp_ = measurement.timestamp_s;
      odom_initialized_ = true;
      return;
    }
    const double distance_delta = measurement.distance_m - last_measured_distance_m_;
    const double heading_delta = std::remainder(
        measurement.heading_change_rad - last_heading_change_rad_, 2.0 * std::numbers::pi);
    const double midpoint_heading = estimated_heading_rad_ + heading_delta * 0.5;
    estimated_position_.x_m += distance_delta * std::cos(midpoint_heading);
    estimated_position_.y_m += distance_delta * std::sin(midpoint_heading);
    const double dt = measurement.timestamp_s - last_odom_stamp_;
    if (dt > 0.0) estimated_yaw_rate_ = heading_delta / dt;
    if (imu) estimated_heading_rad_ = imu->heading_rad;
    else estimated_heading_rad_ += heading_delta;
    last_measured_distance_m_ = measurement.distance_m;
    last_heading_change_rad_ = measurement.heading_change_rad;
  }

  void publish_dynamic_transform(const nav_msgs::msg::Odometry& odom) {
    geometry_msgs::msg::TransformStamped tf;
    tf.header = odom.header;
    tf.child_frame_id = odom.child_frame_id;
    tf.transform.translation.x = odom.pose.pose.position.x;
    tf.transform.translation.y = odom.pose.pose.position.y;
    tf.transform.rotation = odom.pose.pose.orientation;
    dynamic_tf_->sendTransform(tf);
  }

  AutonomySimulation& simulation_;
  const AutonomyScenario& scenario_;
  double timeout_s_;
  CommandWatchdog watchdog_;
  rclcpp::Publisher<sensor_msgs::msg::LaserScan>::SharedPtr scan_pub_;
  rclcpp::Publisher<sensor_msgs::msg::Imu>::SharedPtr imu_pub_;
  rclcpp::Publisher<nav_msgs::msg::Odometry>::SharedPtr odom_pub_;
  rclcpp::Publisher<sensor_msgs::msg::NavSatFix>::SharedPtr gnss_pub_;
  rclcpp::Publisher<nav_msgs::msg::Path>::SharedPtr route_pub_;
  rclcpp::Publisher<rosgraph_msgs::msg::Clock>::SharedPtr clock_pub_;
  rclcpp::Subscription<geometry_msgs::msg::Twist>::SharedPtr cmd_sub_;
  std::shared_ptr<tf2_ros::StaticTransformBroadcaster> static_tf_;
  std::shared_ptr<tf2_ros::TransformBroadcaster> dynamic_tf_;
  std::uint64_t scan_count_{}, imu_count_{}, odom_count_{}, gnss_count_{};
  double last_scan_stamp_{-1.0}, last_imu_stamp_{-1.0}, last_odom_stamp_{-1.0}, last_gnss_stamp_{-1.0};
  bool odom_initialized_{};
  double last_measured_distance_m_{}, last_heading_change_rad_{};
  double estimated_heading_rad_{}, estimated_yaw_rate_{};
  airside::Vec2 estimated_position_{};
};

void print_result(const airside::autonomy::AutonomyRun& run, const BridgeNode& bridge,
                  std::optional<double> run_limit) {
  const auto& metrics = run.metrics;
  std::cout << std::fixed << std::setprecision(3)
      << "RampLab ROS 2 autonomy bridge result\n"
      << "Mission: Depot -> Gate A2\n"
      << "Result: " << airside::autonomy::to_string(metrics.result) << '\n'
      << "Completion time: " << metrics.completion_time_s << " s\n"
      << "Distance traveled: " << metrics.distance_traveled_m << " m\n"
      << "Mean route error: " << metrics.mean_route_error_m << " m\n"
      << "Maximum route error: " << metrics.maximum_route_error_m << " m\n"
      << "Minimum obstacle clearance: " << metrics.minimum_obstacle_clearance_m << " m\n"
      << "Final speed: " << run.final_state.speed_mps << " m/s\n"
      << "Final local position: x=" << run.final_state.position.x_m
      << " m, y=" << run.final_state.position.y_m << " m\n"
      << "Emergency stops: " << metrics.emergency_stops << '\n'
      << "Collisions: " << metrics.collision_count << '\n'
      << "Command timeout activations: " << bridge.timeout_count() << '\n'
      << "Latest command age: " << bridge.latest_command_age() << " s\n"
      << "Published messages: scan=" << bridge.count_scan() << " imu=" << bridge.count_imu()
      << " odom=" << bridge.count_odom() << " gnss=" << bridge.count_gnss() << '\n'
      << "Trajectory digest: " << metrics.trajectory_digest << '\n';
  if (run_limit) std::cout << "Stopped at requested simulation limit: " << *run_limit << " s\n";
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto options = parse_options(argc, argv);
    auto scenario = airside::autonomy::load_scenario(options.scenario);
    const auto seed = options.seed.value_or(scenario.default_seed);
    AutonomySimulation simulation(scenario, seed);
    rclcpp::init(argc, argv);
    auto bridge = std::make_shared<BridgeNode>(simulation, scenario, options.command_timeout_s);
    bridge->initialize_transforms();
    ExternalCommand command;
    bridge->publish_initial();
    RCLCPP_INFO(bridge->get_logger(),
        "ROS 2 bridge active; controller source=external ROS process, pace=%.2fx, timeout=%.3f sim s",
        options.realtime_factor, options.command_timeout_s);
    const auto start = std::chrono::steady_clock::now();
    while (rclcpp::ok() && !simulation.finished() &&
        (!options.max_simulation_s || simulation.time_s() + 1e-9 < *options.max_simulation_s)) {
      const auto next_wall_time = start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
          std::chrono::duration<double>((simulation.time_s() + scenario.timestep_s) /
              options.realtime_factor));
      bridge->spin_once();
      bridge->advance_one(command);
      std::this_thread::sleep_until(next_wall_time);
    }
    const bool limited = options.max_simulation_s && !simulation.finished();
    const auto result = simulation.result();
    print_result(result, *bridge, limited ? options.max_simulation_s : std::nullopt);
    const bool clean_limited_stop = limited && result.metrics.collision_count == 0;
    rclcpp::shutdown();
    return (result.metrics.result == airside::autonomy::MissionResult::Success || clean_limited_stop) ? 0 : 2;
  } catch (const std::exception& error) {
    std::cerr << "ramplab_ros2_bridge: " << error.what() << '\n';
    if (rclcpp::ok()) rclcpp::shutdown();
    return 1;
  }
}
