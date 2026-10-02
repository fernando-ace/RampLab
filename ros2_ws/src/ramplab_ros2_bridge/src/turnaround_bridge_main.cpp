#include "ramplab_ros2_bridge/turnaround_json.hpp"

#include "airside/operations/simulation.hpp"
#include "airside/scenario/scenario_loader.hpp"

#include "rclcpp/rclcpp.hpp"
#include "std_msgs/msg/string.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

class TurnaroundBridge final : public rclcpp::Node, public airside::ISimulationEventSink {
 public:
  TurnaroundBridge(const std::filesystem::path& scenario_path, std::optional<std::uint64_t> seed)
      : Node("ramplab_turnaround_bridge") {
    auto scenario = airside::load_scenario(scenario_path);
    const auto resolved_seed = airside::resolve_seed(scenario, seed);
    simulation_ = std::make_unique<airside::Simulation>(std::move(scenario), resolved_seed);
    simulation_->add_event_sink(*this);
    state_pub_ = create_publisher<std_msgs::msg::String>(
        "/ramplab/turnaround/state", rclcpp::QoS(1).reliable().transient_local());
    events_pub_ = create_publisher<std_msgs::msg::String>(
        "/ramplab/turnaround/events", rclcpp::QoS(1000).reliable().transient_local());
    std_msgs::msg::String initial_state;
    initial_state.data = ramplab_ros2_bridge::turnaround_state_json(simulation_->snapshot());
    state_pub_->publish(initial_state);
    timer_ = create_wall_timer(std::chrono::milliseconds{50}, [this] { advance_one_event(); });
    RCLCPP_INFO(get_logger(), "Turnaround observer loaded scenario %s with seed %llu",
        scenario_path.string().c_str(), static_cast<unsigned long long>(resolved_seed));
  }

  void on_event(const airside::SimulationEventRecord& event) noexcept override {
    if (event.turnaround_id.empty()) return;
    try {
      std_msgs::msg::String message;
      message.data = ramplab_ros2_bridge::turnaround_event_json(event);
      events_pub_->publish(message);
    } catch (...) {
      RCLCPP_ERROR(get_logger(), "Could not publish turnaround event %llu",
          static_cast<unsigned long long>(event.sequence));
    }
  }

 private:
  void advance_one_event() {
    if (!simulation_) return;
    if (simulation_->finished()) {
      publish_state();
      return;
    }
    // Fleet movement contributes many fixed-step events. Advance a bounded
    // batch per observer tick so multi-aircraft scenarios remain observable
    // in wall time while every transition still comes from the core queue.
    constexpr std::size_t kEventsPerTick = 1000;
    for (std::size_t index = 0; index < kEventsPerTick && simulation_->advance(); ++index) {
      if (simulation_->finished()) break;
    }
    publish_state();
  }

  void publish_state() {
    std_msgs::msg::String state;
    state.data = ramplab_ros2_bridge::turnaround_state_json(simulation_->snapshot());
    state_pub_->publish(state);
  }

  std::unique_ptr<airside::Simulation> simulation_;
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr state_pub_;
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr events_pub_;
  rclcpp::TimerBase::SharedPtr timer_;
};

}  // namespace

int main(int argc, char** argv) {
  std::filesystem::path scenario{"scenarios/turnaround_normal.yaml"};
  std::optional<std::uint64_t> seed;
  try {
    for (int index = 1; index < argc; ++index) {
      const std::string argument{argv[index]};
      if ((argument == "--scenario" || argument == "--seed") && index + 1 >= argc) {
        throw std::invalid_argument(argument + " requires a value");
      }
      if (argument == "--scenario") scenario = argv[++index];
      else if (argument == "--seed") seed = std::stoull(argv[++index]);
    }
    rclcpp::init(0, nullptr);
    rclcpp::spin(std::make_shared<TurnaroundBridge>(scenario, seed));
    rclcpp::shutdown();
    return 0;
  } catch (const std::exception& error) {
    if (rclcpp::ok()) rclcpp::shutdown();
    std::fprintf(stderr, "ramplab_ros2_turnaround_bridge: %s\n", error.what());
    return 1;
  }
}
