#include "airside/autonomy/fleet.hpp"

#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/string.hpp>

#include <chrono>
#include <algorithm>
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
  FleetBridge(const std::string& scenario_path, std::uint64_t seed, std::size_t steps_per_tick)
      : Node("ramplab_fleet_bridge"),
        scenario_(airside::autonomy::load_fleet_scenario(scenario_path)),
        simulation_(scenario_, seed == 0 ? scenario_.default_seed : seed),
        steps_per_tick_(steps_per_tick),
        state_pub_(create_publisher<std_msgs::msg::String>(
            "/ramplab/fleet/state", rclcpp::QoS(1).reliable().transient_local())),
        events_pub_(create_publisher<std_msgs::msg::String>(
            "/ramplab/fleet/traffic_events", rclcpp::QoS(100).reliable())),
        dispatch_events_pub_(create_publisher<std_msgs::msg::String>(
            "/ramplab/fleet/dispatch_events", rclcpp::QoS(100).reliable())) {
    timer_ = create_wall_timer(std::chrono::milliseconds(20), [this] { tick(); });
  }

 private:
  void tick() {
    for (std::size_t i = 0; i < steps_per_tick_ && !simulation_.finished(); ++i)
      (void)simulation_.advance();
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
          << ",\"dispatch\":{\"requests_created\":" << metrics.dispatch.requests_created
          << ",\"requests_completed\":" << metrics.dispatch.requests_completed
          << ",\"requests_failed\":" << metrics.dispatch.requests_failed
          << ",\"assignments\":" << metrics.dispatch.assignments
          << ",\"reassignments\":" << metrics.dispatch.reassignments
          << ",\"aging_activations\":" << metrics.dispatch.aging_activations
          << ",\"average_queue_wait_s\":" << metrics.dispatch.average_queue_wait_s
          << ",\"maximum_queue_wait_s\":" << metrics.dispatch.maximum_queue_wait_s
          << ",\"unfinished_requests\":" << metrics.dispatch.unfinished_requests
          << ",\"requests\":[";
    for (std::size_t i = 0; i < metrics.dispatch.requests.size(); ++i) {
      const auto& task = metrics.dispatch.requests[i];
      if (i) state << ',';
      state << "{\"request_id\":" << quote(task.request.id.value)
            << ",\"type\":" << quote(airside::autonomy::to_string(task.request.kind))
            << ",\"required_capability\":" << quote(task.request.required_capability)
            << ",\"origin\":" << quote(task.request.origin)
            << ",\"destination\":" << quote(task.request.destination)
            << ",\"priority\":" << task.request.priority
            << ",\"release_time_s\":" << task.request.release_time_s
            << ",\"state\":" << quote(airside::autonomy::to_string(task.state))
            << ",\"assigned_vehicle\":" << (task.assigned_vehicle ? quote(task.assigned_vehicle->value) : "null")
            << ",\"queue_wait_s\":" << task.queue_wait_s
            << ",\"reassignments\":" << task.reassignments << '}';
    }
    state << "]},\"vehicles\":[";
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
      if (vehicle.current_request) state << ",\"current_request_id\":" << quote(vehicle.current_request->value);
      state << ",\"dispatch_state\":" << quote(airside::autonomy::to_string(vehicle.dispatch_state))
            << ",\"tasks_completed\":" << vehicle.tasks_completed;
      const auto vehicle_metrics = std::ranges::find(metrics.vehicles, vehicle.id,
          [](const auto& item) { return item.id; });
      if (vehicle_metrics != metrics.vehicles.end())
        state << ",\"busy_time_s\":" << vehicle_metrics->busy_time_s
              << ",\"idle_time_s\":" << vehicle_metrics->idle_time_s
              << ",\"utilization\":" << vehicle_metrics->utilization;
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
    while (dispatch_event_count_ < metrics.dispatch.events.size()) {
      const auto& event = metrics.dispatch.events[dispatch_event_count_++];
      std_msgs::msg::String event_message;
      std::ostringstream payload;
      payload << std::setprecision(17) << "{\"simulation_time_s\":" << event.time_s
              << ",\"kind\":" << quote(airside::autonomy::to_string(event.kind))
              << ",\"request_id\":" << quote(event.request.value)
              << ",\"vehicle_id\":" << quote(event.vehicle.value)
              << ",\"detail\":" << quote(event.detail)
              << ",\"effective_priority\":" << event.effective_priority
              << ",\"route_distance_m\":" << event.route_distance_m << '}';
      event_message.data = payload.str();
      dispatch_events_pub_->publish(event_message);
    }
  }

  airside::autonomy::FleetScenario scenario_;
  airside::autonomy::FleetSimulation simulation_;
  std::size_t steps_per_tick_{};
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr state_pub_;
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr events_pub_;
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr dispatch_events_pub_;
  rclcpp::TimerBase::SharedPtr timer_;
  std::size_t event_count_{};
  std::size_t dispatch_event_count_{};
};
}

int main(int argc, char** argv) {
  try {
    std::string scenario = "scenarios/autonomy_fleet.yaml";
    std::uint64_t seed = 0;
    std::size_t steps_per_tick = 1;
    for (int i = 1; i < argc; ++i) {
      const std::string_view option{argv[i]};
      if (option == "--scenario" && i + 1 < argc) scenario = argv[++i];
      else if (option == "--seed" && i + 1 < argc) seed = std::stoull(argv[++i]);
      else if (option == "--steps-per-tick" && i + 1 < argc) steps_per_tick = std::stoull(argv[++i]);
      else if (option == "--help") {
        std::cout << "ramplab_ros2_fleet_bridge [--scenario FILE] [--seed N] [--steps-per-tick N]\n";
        return 0;
      } else if (option != "--ros-args" && option != "--") {
        throw std::invalid_argument("unknown or incomplete option: " + std::string(option));
      }
    }
    rclcpp::init(argc, argv);
    if (steps_per_tick == 0 || steps_per_tick > 100000)
      throw std::invalid_argument("--steps-per-tick must be in [1,100000]");
    rclcpp::spin(std::make_shared<FleetBridge>(scenario, seed, steps_per_tick));
    rclcpp::shutdown();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "ramplab_ros2_fleet_bridge: " << error.what() << '\n';
    return 1;
  }
}
