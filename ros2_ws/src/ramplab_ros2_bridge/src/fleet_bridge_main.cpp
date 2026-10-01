#include "airside/autonomy/fleet.hpp"

#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/string.hpp>

#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
std::string quote(std::string_view value) {
  std::ostringstream out;
  out << '"';
  for (const unsigned char c : value) {
    if (c == '"' || c == '\\') out << '\\' << static_cast<char>(c);
    else if (c == '\n') out << "\\n";
    else if (c >= 0x20) out << static_cast<char>(c);
  }
  out << '"';
  return out.str();
}

class FleetBridge final : public rclcpp::Node {
 public:
  FleetBridge(const std::string& scenario_path, std::uint64_t seed)
      : Node("ramplab_fleet_bridge"),
        scenario_(airside::autonomy::load_fleet_scenario(scenario_path)),
        simulation_(scenario_.vehicle_scenario, scenario_.missions,
                    seed == 0 ? scenario_.default_seed : seed,
                    scenario_.road_events, scenario_.deadlock_persistence_s, scenario_.resource_specific_tie_breaks),
        state_pub_(create_publisher<std_msgs::msg::String>(
            "/ramplab/fleet/state", rclcpp::QoS(1).reliable().transient_local())),
        events_pub_(create_publisher<std_msgs::msg::String>(
            "/ramplab/fleet/traffic_events", rclcpp::QoS(100).reliable())) {
    timer_ = create_wall_timer(std::chrono::milliseconds(20), [this] { tick(); });
  }

 private:
  void tick() {
    if (!simulation_.finished()) (void)simulation_.advance();
    const auto metrics = simulation_.result();
    std::map<airside::autonomy::VehicleId, airside::autonomy::FleetWaitDependency> blockers;
    for (const auto& dependency : metrics.wait_dependencies) blockers[dependency.waiting_vehicle] = dependency;
    std::ostringstream state;
    state << std::setprecision(17) << "{\"simulation_time_s\":" << simulation_.time_s()
          << ",\"missions_completed\":" << metrics.missions_completed
          << ",\"missions_attempted\":" << metrics.missions_attempted
          << ",\"deadlocks\":" << metrics.deadlock_count
          << ",\"recoveries\":" << metrics.recovery_attempts
          << ",\"retreats\":" << metrics.retreat_count
          << ",\"outstanding_reservations\":" << metrics.outstanding_reservations
          << ",\"vehicles\":[";
    const auto snapshots = simulation_.snapshots();
    for (std::size_t i = 0; i < snapshots.size(); ++i) {
      const auto& vehicle = snapshots[i];
      if (i) state << ',';
      state << "{\"vehicle_id\":" << quote(vehicle.id.value)
            << ",\"goal_node\":" << quote(vehicle.goal_node)
            << ",\"x_m\":" << vehicle.autonomy.ground_truth.position.x_m
            << ",\"y_m\":" << vehicle.autonomy.ground_truth.position.y_m
            << ",\"speed_mps\":" << vehicle.autonomy.ground_truth.speed_mps
            << ",\"waiting\":" << (vehicle.waiting ? "true" : "false")
            << ",\"finished\":" << (vehicle.autonomy.finished ? "true" : "false")
            << ",\"recovery_state\":" << quote(vehicle.recovery_state)
            << ",\"recovery_resource\":" << quote(vehicle.recovery_resource)
            << ",\"retreat_target_x_m\":" << vehicle.retreat_target.x_m
            << ",\"retreat_target_y_m\":" << vehicle.retreat_target.y_m
            << ",\"retreat_progress_m\":" << vehicle.retreat_progress_m
            << ",\"recovery_attempts\":" << vehicle.recovery_attempts;
      if (const auto dependency = blockers.find(vehicle.id); dependency != blockers.end()) {
        state << ",\"blocking_vehicle\":" << quote(dependency->second.blocking_vehicle.value)
              << ",\"blocked_resource\":" << quote(dependency->second.resource)
              << ",\"wait_duration_s\":" << dependency->second.wait_duration_s;
      }
      state << '}';
    }
    state << "]}";
    std_msgs::msg::String state_message;
    state_message.data = state.str();
    state_pub_->publish(state_message);

    while (event_count_ < metrics.events.size()) {
      const auto& event = metrics.events[event_count_++];
      std_msgs::msg::String event_message;
      std::ostringstream payload;
      payload << std::setprecision(17) << "{\"simulation_time_s\":" << event.time_s
              << ",\"kind\":" << quote(airside::autonomy::to_string(event.kind))
              << ",\"vehicle_id\":" << quote(event.vehicle.value)
              << ",\"other_vehicle_id\":" << quote(event.other.value)
              << ",\"resource\":" << quote(event.resource)
              << ",\"x_m\":" << event.position.x_m << ",\"y_m\":" << event.position.y_m
              << ",\"target_x_m\":" << event.target.x_m << ",\"target_y_m\":" << event.target.y_m
              << ",\"progress_m\":" << event.progress_m << '}';
      event_message.data = payload.str();
      events_pub_->publish(event_message);
    }
  }

  airside::autonomy::FleetScenario scenario_;
  airside::autonomy::FleetSimulation simulation_;
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr state_pub_;
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr events_pub_;
  rclcpp::TimerBase::SharedPtr timer_;
  std::size_t event_count_{};
};
}

int main(int argc, char** argv) {
  try {
    std::string scenario = "scenarios/autonomy_fleet.yaml";
    std::uint64_t seed = 0;
    for (int i = 1; i < argc; ++i) {
      const std::string_view option{argv[i]};
      if (option == "--scenario" && i + 1 < argc) scenario = argv[++i];
      else if (option == "--seed" && i + 1 < argc) seed = std::stoull(argv[++i]);
      else if (option == "--help") {
        std::cout << "ramplab_ros2_fleet_bridge [--scenario FILE] [--seed N]\n";
        return 0;
      } else if (option != "--ros-args" && option != "--") {
        throw std::invalid_argument("unknown or incomplete option: " + std::string(option));
      }
    }
    rclcpp::init(argc, argv);
    rclcpp::spin(std::make_shared<FleetBridge>(scenario, seed));
    rclcpp::shutdown();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "ramplab_ros2_fleet_bridge: " << error.what() << '\n';
    return 1;
  }
}
