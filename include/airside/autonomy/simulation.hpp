#pragma once

#include "airside/operations/simulation.hpp"

#include <cstdint>
#include <memory>
#include <iosfwd>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace airside::autonomy {

struct VehicleState {
    Vec2 position{};
    double heading_rad{0.0};
    double speed_mps{0.0};
    double yaw_rate_radps{0.0};
    double distance_m{0.0};
};

struct VehicleLimits {
    double maximum_speed_mps{5.0};
    double maximum_acceleration_mps2{1.0};
    double maximum_deceleration_mps2{1.5};
    double maximum_yaw_rate_radps{0.8};
    double radius_m{1.0};
};

struct CircleObstacle {
    std::string id;
    Vec2 center{};
    double radius_m{1.0};
};

struct SensorConfig {
    double gnss_hz{5.0};
    double gnss_sigma_m{0.5};
    Vec2 gnss_bias_m{};
    double imu_hz{50.0};
    double imu_heading_sigma_rad{0.005};
    double imu_yaw_rate_sigma_radps{0.005};
    double imu_accel_sigma_mps2{0.03};
    double odometry_hz{20.0};
    double odometry_sigma_mps{0.02};
    double odometry_sigma_m{0.01};
    double lidar_hz{10.0};
    double lidar_fov_rad{3.14159265358979323846};
    std::size_t lidar_beams{181};
    double lidar_min_range_m{0.1};
    double lidar_max_range_m{30.0};
    double lidar_sigma_m{0.01};
};

struct AutonomyScenario {
    std::string name{"autonomy_tug"};
    std::uint64_t default_seed{42};
    Scenario airport;
    std::string start_node{"depot"};
    std::string goal_node{"gate_a2"};
    VehicleState initial_state{};
    VehicleLimits limits{};
    SensorConfig sensors{};
    std::vector<CircleObstacle> obstacles;
    double timestep_s{0.02};
    double goal_tolerance_m{2.0};
    double stopped_speed_mps{0.15};
    double timeout_s{240.0};
    double safety_stop_range_m{2.2};
};

struct GnssMeasurement { double timestamp_s{}; Vec2 position{}; double accuracy_m{}; };
struct ImuMeasurement { double timestamp_s{}; double heading_rad{}; double yaw_rate_radps{}; double longitudinal_accel_mps2{}; };
struct OdometryMeasurement { double timestamp_s{}; double distance_m{}; double speed_mps{}; double heading_change_rad{}; };
struct LidarScan {
    double timestamp_s{};
    double angle_min_rad{};
    double angle_increment_rad{};
    double range_min_m{};
    double range_max_m{};
    std::vector<double> ranges_m;
};

struct SensorFrame {
    double timestamp_s{};
    std::optional<GnssMeasurement> gnss;
    std::optional<ImuMeasurement> imu;
    std::optional<OdometryMeasurement> odometry;
    std::optional<LidarScan> lidar;
};

struct VehicleCommand { double target_speed_mps{}; double target_yaw_rate_radps{}; };
struct MissionState { std::vector<Vec2> waypoints; Vec2 goal{}; VehicleLimits limits{}; };

class IAutonomyController {
public:
    virtual ~IAutonomyController() = default;
    [[nodiscard]] virtual VehicleCommand update(const SensorFrame&, const MissionState&) = 0;
};

class ReferenceController final : public IAutonomyController {
public:
    explicit ReferenceController(double safety_stop_range_m = 2.2);
    [[nodiscard]] VehicleCommand update(const SensorFrame&, const MissionState&) override;
    [[nodiscard]] std::size_t emergency_stops() const noexcept { return emergency_stops_; }
private:
    double safety_stop_range_m_;
    std::size_t emergency_stops_{0};
    Vec2 estimated_position_{};
    double estimated_heading_rad_{0.0};
    bool have_position_{false};
    bool safety_was_active_{false};
    double last_odometry_distance_m_{0.0};
    double last_gnss_timestamp_s_{-1.0};
    double last_odometry_timestamp_s_{-1.0};
    std::size_t target_waypoint_index_{1};
};

enum class MissionResult { Success, Collision, Timeout, ControllerFailure };
struct MissionMetrics {
    MissionResult result{MissionResult::Timeout};
    double completion_time_s{};
    double distance_traveled_m{};
    double path_efficiency{};
    double mean_route_error_m{};
    double maximum_route_error_m{};
    double minimum_obstacle_clearance_m{};
    std::size_t emergency_stops{};
    std::size_t collision_count{};
    std::optional<double> first_collision_time_s;
    std::string collided_obstacle;
    std::size_t gnss_samples{};
    std::size_t imu_samples{};
    std::size_t odometry_samples{};
    std::size_t lidar_scans{};
    std::uint64_t trajectory_digest{};
};
struct AutonomyRun {
    VehicleState final_state{};
    MissionMetrics metrics{};
};
struct AutonomySnapshot {
    double timestamp_s{};
    VehicleState ground_truth{};
    SensorFrame sensors{};
    MissionState mission{};
    std::vector<CircleObstacle> obstacles;
    MissionMetrics metrics{};
    bool finished{};
};

class AutonomySimulation {
public:
    AutonomySimulation(AutonomyScenario scenario, std::uint64_t seed);
    ~AutonomySimulation();
    AutonomySimulation(AutonomySimulation&&) noexcept;
    AutonomySimulation& operator=(AutonomySimulation&&) noexcept;
    AutonomySimulation(const AutonomySimulation&) = delete;
    AutonomySimulation& operator=(const AutonomySimulation&) = delete;
    [[nodiscard]] const MissionState& mission() const noexcept;
    [[nodiscard]] const AirportGraph& graph() const noexcept;
    [[nodiscard]] const std::vector<CircleObstacle>& obstacles() const noexcept;
    [[nodiscard]] const VehicleState& state() const noexcept;
    [[nodiscard]] double time_s() const noexcept;
    [[nodiscard]] bool finished() const noexcept;
    [[nodiscard]] SensorFrame observe() const;
    [[nodiscard]] AutonomySnapshot snapshot() const;
    [[nodiscard]] bool advance(IAutonomyController& controller);
    [[nodiscard]] AutonomyRun result() const;
    [[nodiscard]] AutonomyRun run(IAutonomyController& controller, std::ostream* trajectory_csv = nullptr);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::string to_string(MissionResult result);

}  // namespace airside::autonomy
