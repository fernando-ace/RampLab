#pragma once

#include "airside/operations/simulation.hpp"
#include "airside/autonomy/sensor_model.hpp"

#include <cstdint>
#include <array>
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
    bool operator==(const VehicleState&) const = default;
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

struct SensorExtrinsics { double x_m{}, y_m{}, z_m{}, yaw_rad{}; };

struct SensorConfig {
    double gnss_hz{10.0};
    double gnss_sigma_m{0.5};
    Vec2 gnss_bias_m{};
    double imu_hz{50.0};
    double imu_heading_sigma_rad{0.005};
    double imu_yaw_rate_sigma_radps{0.005};
    double imu_accel_sigma_mps2{0.03};
    double odometry_hz{50.0};
    double odometry_sigma_mps{0.02};
    double odometry_sigma_m{0.01};
    double lidar_hz{10.0};
    double camera_hz{20.0};
    double lidar_fov_rad{3.14159265358979323846};
    std::size_t lidar_beams{181};
    double lidar_min_range_m{0.1};
    double lidar_max_range_m{30.0};
    double lidar_sigma_m{0.01};
    SensorTimingConfig gnss_timing{};
    SensorTimingConfig imu_timing{};
    SensorTimingConfig odometry_timing{};
    SensorTimingConfig lidar_timing{};
    SensorTimingConfig camera_timing{};
    SensorExtrinsics gnss_extrinsics{0.0, 0.0, 0.20, 0.0};
    SensorExtrinsics imu_extrinsics{};
    SensorExtrinsics odometry_extrinsics{};
    SensorExtrinsics lidar_extrinsics{3.40, 0.0, -0.30, 0.0};
    SensorExtrinsics camera_extrinsics{3.40, 0.0, 0.0, 0.0};
};

// Covariance is row-major for state [east, north, yaw, forward speed].
struct EstimatorConfig {
    double initial_position_variance_m2{4.0};
    double initial_heading_variance_rad2{0.04};
    double initial_speed_variance_m2ps2{1.0};
    double position_process_noise_m2ps{0.04};
    double heading_process_noise_rad2ps{0.0025};
    double speed_process_noise_m2ps3{0.16};
    double gnss_sigma_m{0.5};
    double imu_heading_sigma_rad{0.02};
    double imu_yaw_rate_sigma_radps{0.02};
    double odometry_speed_sigma_mps{0.1};
    double odometry_heading_sigma_rad{0.05};
    double gnss_nis_gate{9.210340371976184};
    double maximum_measurement_age_s{0.5};
    double degraded_position_sigma_m{3.0};
    double unsafe_position_sigma_m{8.0};
    double degraded_heading_sigma_rad{0.5};
    double unsafe_heading_sigma_rad{1.2};
    double unsafe_without_gnss_s{6.0};
    double unobserved_stop_deceleration_mps2{1.5};
};
enum class EstimatorHealth { Uninitialized, Healthy, Degraded, Unsafe, Invalid };
enum class WheelHealth { Nominal, Suspect, Degraded };
enum class GnssRecoveryState { Tracking, Inconsistent, Reacquiring, Recovered };
struct EstimatedState {
    double timestamp_s{};
    Vec2 position{};
    double heading_rad{};
    double speed_mps{};
    std::array<double, 16> covariance{};
    bool initialized{};
    EstimatorHealth health{EstimatorHealth::Uninitialized};
    double position_uncertainty_m{}, heading_uncertainty_rad{}, time_since_gnss_s{};
    std::size_t gnss_accepted{}, gnss_rejected{}, stale_rejected{}, gate_activations{}, gnss_reject_streak{};
    double maximum_gnss_innovation_m{}, last_gnss_nis{};
    double maximum_gnss_nis{};
    WheelHealth wheel_health{WheelHealth::Nominal};
    GnssRecoveryState gnss_recovery{GnssRecoveryState::Tracking};
    std::size_t wheel_inconsistency_count{}, wheel_health_transitions{}, wheel_downweighted{}, reacquisition_attempts{}, reacquisition_successes{}, reacquisition_candidates_rejected{};
    double localization_degraded_time_s{};
};

enum class SensorKind { Gnss, Imu, Odometry, Lidar };
enum class SensorFaultKind { Dropout, Noise, Bias, RangeLimit, Obstruction, Scale, Drift, Delay, PacketLoss, BurstLoss };
struct FaultEventRecord { std::size_t fault_index{}; double time_s{}; std::string event; bool operator==(const FaultEventRecord&) const = default; };
struct EstimatorEventRecord { double time_s{}; std::string event; bool operator==(const EstimatorEventRecord&) const = default; };
struct SensorFault {
    SensorKind sensor{SensorKind::Gnss};
    SensorFaultKind kind{SensorFaultKind::Dropout};
    double start_s{};
    double duration_s{};
    double magnitude{};
    Vec2 offset{};
    double angle_min_rad{};
    double angle_max_rad{};
    double probability{};
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
    EstimatorConfig estimator{};
    bool estimator_enabled{true};
    std::vector<CircleObstacle> obstacles;
    double timestep_s{0.02};
    double goal_tolerance_m{2.0};
    double stopped_speed_mps{0.15};
    double timeout_s{240.0};
    double safety_stop_range_m{2.2};
    double localization_timeout_s{3.0};
    double perception_timeout_s{0.5};
    std::vector<SensorFault> faults;
};

struct GnssMeasurement { double timestamp_s{}; Vec2 position{}; double accuracy_m{}; ObservationMetadata metadata{}; };
struct ImuMeasurement { double timestamp_s{}; double heading_rad{}; double yaw_rate_radps{}; double longitudinal_accel_mps2{}; ObservationMetadata metadata{}; };
struct OdometryMeasurement { double timestamp_s{}; double distance_m{}; double speed_mps{}; double heading_change_rad{}; ObservationMetadata metadata{}; };
struct LidarScan {
    double timestamp_s{};
    double angle_min_rad{};
    double angle_increment_rad{};
    double range_min_m{};
    double range_max_m{};
    std::vector<double> ranges_m;
    ObservationMetadata metadata{};
};
struct CameraFrameMetadata {
    ObservationMetadata metadata{};
    std::uint32_t width{320};
    std::uint32_t height{180};
    double horizontal_fov_rad{1.5707963267948966};
};

struct SensorFrame {
    double timestamp_s{};
    std::optional<GnssMeasurement> gnss;
    std::optional<ImuMeasurement> imu;
    std::optional<OdometryMeasurement> odometry;
    std::optional<LidarScan> lidar;
    std::optional<CameraFrameMetadata> camera;
    std::optional<EstimatedState> estimate;
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
    explicit ReferenceController(double safety_stop_range_m = 2.2, double perception_timeout_s = 0.5, double localization_timeout_s = 3.0);
    [[nodiscard]] VehicleCommand update(const SensorFrame&, const MissionState&) override;
    [[nodiscard]] std::size_t emergency_stops() const noexcept { return emergency_stops_; }
    [[nodiscard]] std::size_t degraded_mode_entries() const noexcept { return degraded_mode_entries_; }
    [[nodiscard]] std::size_t safety_stop_entries() const noexcept { return safety_stop_entries_; }
    [[nodiscard]] double degraded_stop_time_s() const noexcept { return degraded_stop_time_s_; }
    [[nodiscard]] bool has_estimated_position() const noexcept { return have_position_; }
    [[nodiscard]] Vec2 estimated_position() const noexcept { return estimated_position_; }
    [[nodiscard]] double estimated_heading_rad() const noexcept { return estimated_heading_rad_; }
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
    std::size_t degraded_mode_entries_{}, safety_stop_entries_{};
    double perception_timeout_s_{0.5}, localization_timeout_s_{3.0};
    double degraded_since_s_{-1.0}, degraded_stop_time_s_{};
    double last_lidar_timestamp_s_{-1.0}, last_imu_timestamp_s_{-1.0}, safety_stop_since_s_{-1.0};
    double last_update_timestamp_s_{-1.0};
    bool degraded_active_{}, safety_due_to_degraded_sensing_{};
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
    std::size_t lidar_hit_returns{};
    std::size_t messages_dropped{};
    std::size_t messages_delayed{};
    std::size_t gnss_dropped{}, imu_dropped{}, odometry_dropped{}, lidar_dropped{};
    std::size_t gnss_delayed{}, imu_delayed{}, odometry_delayed{}, lidar_delayed{};
    std::size_t gnss_delivered{}, imu_delivered{}, odometry_delivered{}, lidar_delivered{};
    double final_x_m{}, final_y_m{}, final_heading_rad{}, final_speed_mps{};
    std::size_t degraded_mode_entries{};
    std::size_t safety_stop_entries{};
    double time_stopped_degraded_s{};
    double unavailable_duration_s{};
    double maximum_delay_s{};
    double mean_position_error_m{}, rms_position_error_m{}, maximum_position_error_m{}, final_position_error_m{};
    double mean_heading_error_rad{}, maximum_heading_error_rad{}, final_heading_error_rad{};
    double estimator_initialization_time_s{-1.0};
    double estimator_healthy_time_s{}, estimator_degraded_time_s{}, estimator_unsafe_time_s{};
    double maximum_position_uncertainty_m{}, maximum_heading_uncertainty_rad{}, maximum_gnss_nis{};
    std::size_t gnss_updates_accepted{}, gnss_updates_rejected{}, stale_measurements_rejected{}, gnss_gate_activations{};
    std::size_t estimator_uncertainty_safety_stops{};
    std::size_t wheel_inconsistency_count{}, wheel_health_transitions{}, wheel_downweighted{}, gnss_reacquisition_attempts{}, gnss_reacquisition_successes{}, gnss_reacquisition_candidates_rejected{};
    double localization_degraded_time_s{}, estimator_safety_stop_time_s{}, gnss_recovery_latency_s{-1.0};
    WheelHealth final_wheel_health{WheelHealth::Nominal};
    GnssRecoveryState final_gnss_recovery{GnssRecoveryState::Tracking};
    std::vector<EstimatorEventRecord> estimator_events;
    std::vector<double> fault_activation_times_s;
    std::vector<double> fault_deactivation_times_s;
    std::vector<FaultEventRecord> fault_events;
    std::uint64_t trajectory_digest{};
    std::uint64_t sensor_stream_digest{};
    std::uint64_t sensor_stream_records{};
    bool operator==(const MissionMetrics&) const = default;
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
    AutonomySimulation(AutonomyScenario scenario, std::uint64_t seed, std::uint64_t fault_seed = 0);
    ~AutonomySimulation();
    AutonomySimulation(AutonomySimulation&&) noexcept;
    AutonomySimulation& operator=(AutonomySimulation&&) noexcept;
    AutonomySimulation(const AutonomySimulation&) = delete;
    AutonomySimulation& operator=(const AutonomySimulation&) = delete;
    [[nodiscard]] const MissionState& mission() const noexcept;
    [[nodiscard]] const AirportGraph& graph() const noexcept;
    [[nodiscard]] const std::vector<CircleObstacle>& obstacles() const noexcept;
    [[nodiscard]] const VehicleState& state() const noexcept;
    [[nodiscard]] const EstimatedState& estimated_state() const noexcept;
    [[nodiscard]] double time_s() const noexcept;
    [[nodiscard]] bool finished() const noexcept;
    [[nodiscard]] SensorFrame observe() const;
    [[nodiscard]] AutonomySnapshot snapshot() const;
    [[nodiscard]] const AutonomyScenario& scenario() const noexcept;
    void set_edge_available(EdgeId edge, bool available);
    void set_route(std::vector<Vec2> waypoints);
    [[nodiscard]] bool advance(IAutonomyController& controller);
    [[nodiscard]] bool advance_with_command(VehicleCommand command, const ReferenceController& controller);
    [[nodiscard]] bool advance_with_reverse_command(VehicleCommand command, const ReferenceController& controller);
    [[nodiscard]] AutonomyRun result() const;
    [[nodiscard]] AutonomyRun run(IAutonomyController& controller, std::ostream* trajectory_csv = nullptr,
                                  std::ostream* sensor_jsonl = nullptr);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::string to_string(MissionResult result);

}  // namespace airside::autonomy
