#pragma once

#include "airside/autonomy/simulation.hpp"

#include <algorithm>
#include <cstddef>
#include <cmath>

namespace ramplab_ros2_bridge {

class CommandWatchdog {
public:
  struct Decision {
    airside::autonomy::VehicleCommand command{};
    double age_s{};
    bool timed_out{};
    bool timeout_activated{};
  };

  CommandWatchdog(double timeout_s, double start_time_s)
      : timeout_s_(timeout_s), last_command_time_s_(start_time_s) {}

  // Twist has no header timestamp, so freshness begins when the bridge receives it.
  [[nodiscard]] bool accept(airside::autonomy::VehicleCommand command,
                            double simulation_time_s) noexcept {
    if (!std::isfinite(simulation_time_s) || simulation_time_s < last_command_time_s_) return false;
    command_ = command;
    last_command_time_s_ = simulation_time_s;
    received_command_ = true;
    const bool recovered = timed_out_;
    timed_out_ = false;
    return recovered;
  }

  [[nodiscard]] Decision poll(double simulation_time_s) noexcept {
    const double age = std::max(0.0, simulation_time_s - last_command_time_s_);
    const bool expired = age > timeout_s_;
    const bool activated = expired && !timed_out_;
    if (activated) {
      timed_out_ = true;
      ++timeout_count_;
    }
    return {expired ? airside::autonomy::VehicleCommand{} : command_, age, expired, activated};
  }

  [[nodiscard]] std::size_t timeout_count() const noexcept { return timeout_count_; }
  [[nodiscard]] double age(double simulation_time_s) const noexcept {
    return std::max(0.0, simulation_time_s - last_command_time_s_);
  }
  [[nodiscard]] bool received_command() const noexcept { return received_command_; }

private:
  double timeout_s_{};
  double last_command_time_s_{};
  airside::autonomy::VehicleCommand command_{};
  std::size_t timeout_count_{};
  bool received_command_{};
  bool timed_out_{};
};

}  // namespace ramplab_ros2_bridge
