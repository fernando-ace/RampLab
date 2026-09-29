#pragma once

#include "airside/autonomy/simulation.hpp"

#include <array>
#include <cstddef>

namespace airside::autonomy {

class StateEstimator2D {
public:
    explicit StateEstimator2D(EstimatorConfig config = {});
    [[nodiscard]] const EstimatedState& update(const SensorFrame& measurements);
    [[nodiscard]] const EstimatedState& state() const noexcept { return state_; }
    [[nodiscard]] const EstimatorConfig& config() const noexcept { return config_; }
    void reset() noexcept;
private:
    EstimatorConfig config_;
    EstimatedState state_{};
    double previous_time_s_{-1.0};
    double previous_odom_distance_m_{}, previous_odom_heading_change_rad_{};
    double last_odom_heading_estimate_rad_{}, odometry_heading_origin_rad_{};
    double last_imu_stamp_s_{-1.0}, last_odom_stamp_s_{-1.0}, last_gnss_stamp_s_{-1.0};
    double last_gnss_time_s_{-1.0};
    bool have_odom_{}, have_imu_{}, have_odom_heading_origin_{};
    void refresh_health(double now_s) noexcept;
};

[[nodiscard]] const char* to_string(EstimatorHealth health) noexcept;

} // namespace airside::autonomy
