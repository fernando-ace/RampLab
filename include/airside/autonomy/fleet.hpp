#pragma once

#include "airside/autonomy/simulation.hpp"
#include "airside/autonomy/service_request.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace airside::autonomy {

struct FleetMission {
    VehicleId id;
    std::string start_node;
    std::string goal_node;
    int priority{};
    std::vector<SensorFault> faults;
};

struct FleetScenario {
    std::string name;
    std::uint64_t default_seed{42};
    AutonomyScenario vehicle_scenario;
    std::vector<FleetMission> missions;
    std::vector<DispatchVehicle> dispatch_fleet;
    std::vector<ServiceRequest> service_requests;
    double dispatch_aging_interval_s{30.0};
    std::vector<RoadAvailabilityEvent> road_events;
    double deadlock_persistence_s{2.0};
    bool resource_specific_tie_breaks{false};
};

[[nodiscard]] FleetScenario load_fleet_scenario(const std::filesystem::path& path);

enum class TrafficEventKind { Request, Granted, Deferred, Waiting, WaitEnded, EnteredConflict, ReleasedConflict, Collision, DeadlockDetected, DeadlockRecovery, RecoveryResolved, Retreat, RetreatSelected, RetreatStarted, RetreatProgress, RetreatResourceReleased, RetreatCompleted, MissionResumed, RetreatFailed, Reroute, RoadClosed, RoadReopened, NearConflict, ForcedSafetyStop };
struct ReservationRequest {
    VehicleId vehicle;
    double time_s{};
    int priority{};
};

// Deterministic exclusive-resource arbiter with timestamp, priority, and ID ordering.
class TrafficReservationTable {
public:
    explicit TrafficReservationTable(bool resource_specific_tie_breaks = false)
        : resource_specific_tie_breaks_(resource_specific_tie_breaks) {}
    [[nodiscard]] bool request(std::string resource, ReservationRequest request);
    [[nodiscard]] std::optional<VehicleId> request_batch(
        std::string resource, std::vector<ReservationRequest> requests);
    void retain_waiters(const std::string& resource, const std::vector<VehicleId>& active_vehicles);
    [[nodiscard]] bool release(const std::string& resource, const VehicleId& vehicle);
    [[nodiscard]] std::optional<VehicleId> owner(const std::string& resource) const;
    [[nodiscard]] std::vector<std::pair<std::string, VehicleId>> held_resources() const;
    [[nodiscard]] std::size_t starvation_preventions() const noexcept { return starvation_preventions_; }

private:
    struct Entry { VehicleId owner; ReservationRequest request; };
    std::map<std::string, Entry> held_;
    std::map<std::string, std::vector<ReservationRequest>> waiting_;
    std::size_t starvation_preventions_{};
    bool resource_specific_tie_breaks_{};
};

// Returns the sorted set of vehicles that participate in a directed wait-for cycle.
[[nodiscard]] std::vector<VehicleId> find_deadlocked_vehicles(
    const std::map<VehicleId, std::vector<VehicleId>>& waits_for);

struct TrafficEvent {
    double time_s{};
    TrafficEventKind kind{};
    VehicleId vehicle;
    VehicleId other;
    std::string resource;
    Vec2 position{};
    Vec2 target{};
    double progress_m{};
    bool operator==(const TrafficEvent&) const = default;
};

struct FleetWaitDependency {
    VehicleId waiting_vehicle;
    VehicleId blocking_vehicle;
    std::string resource;
    double wait_duration_s{};
    bool operator==(const FleetWaitDependency&) const = default;
};
struct FleetRecoveryCandidate {
    VehicleId vehicle;
    int mission_priority{};
    double accumulated_wait_s{};
};
[[nodiscard]] VehicleId choose_recovery_vehicle(std::vector<FleetRecoveryCandidate> candidates);

struct FleetVehicleResult {
    VehicleId id;
    std::string start_node;
    std::string goal_node;
    MissionMetrics metrics;
    VehicleState final_state;
    std::size_t tasks_completed{};
    double total_distance_m{};
    double busy_time_s{};
    double idle_time_s{};
    double utilization{};
    bool operator==(const FleetVehicleResult&) const = default;
};
struct FleetVehicleSnapshot {
    VehicleId id;
    std::string goal_node;
    std::optional<ServiceRequestId> current_request;
    DispatchVehicleState dispatch_state{DispatchVehicleState::Idle};
    std::vector<std::string> capabilities;
    bool waiting{};
    AutonomySnapshot autonomy;
    std::string recovery_state;
    std::string recovery_resource;
    Vec2 retreat_target{};
    double retreat_progress_m{};
    std::size_t recovery_attempts{};
    std::size_t tasks_completed{};
    double total_distance_m{};
};

struct FleetMetrics {
    std::size_t vehicle_count{};
    std::size_t missions_attempted{};
    std::size_t missions_completed{};
    std::size_t safe_timeouts{};
    std::size_t collisions{};
    double minimum_separation_m{};
    double total_distance_m{};
    double total_mission_time_s{};
    double cumulative_waiting_time_s{};
    double traffic_waiting_time_s{};
    double safety_stop_time_s{};
    std::size_t reservation_requests{};
    std::size_t reservation_contentions{};
    std::size_t deadlock_count{};
    std::size_t deadlocks_resolved{};
    std::size_t recovery_attempts{};
    std::size_t reroutes{};
    std::size_t road_closure_replans{};
    std::size_t retreat_count{};
    std::size_t reservation_denials{};
    std::size_t outstanding_reservations{};
    std::size_t starvation_preventions{};
    double maximum_resource_wait_s{};
    double mean_resource_wait_s{};
    std::vector<FleetWaitDependency> wait_dependencies;
    std::vector<VehicleId> deadlocked_vehicles;
    std::size_t near_conflict_events{};
    std::size_t forced_safety_stops{};
    double throughput_per_simulated_hour{};
    std::vector<FleetVehicleResult> vehicles;
    std::vector<TrafficEvent> events;
    FleetDispatchMetrics dispatch;
    std::uint64_t deterministic_digest{};
    bool operator==(const FleetMetrics&) const = default;
};

class FleetSimulation {
public:
    FleetSimulation(AutonomyScenario base, std::vector<FleetMission> missions, std::uint64_t seed,
                    std::vector<RoadAvailabilityEvent> road_events = {}, double deadlock_persistence_s = 2.0,
                    bool resource_specific_tie_breaks = false);
    FleetSimulation(FleetScenario scenario, std::uint64_t seed);
    ~FleetSimulation();
    FleetSimulation(FleetSimulation&&) noexcept;
    FleetSimulation& operator=(FleetSimulation&&) noexcept;
    FleetSimulation(const FleetSimulation&) = delete;
    FleetSimulation& operator=(const FleetSimulation&) = delete;

    [[nodiscard]] bool finished() const noexcept;
    [[nodiscard]] double time_s() const noexcept;
    [[nodiscard]] bool advance();
    [[nodiscard]] FleetMetrics result() const;
    [[nodiscard]] std::vector<FleetVehicleSnapshot> snapshots() const;
    [[nodiscard]] std::vector<FleetWaitDependency> wait_dependencies() const;
    [[nodiscard]] std::vector<VehicleId> deadlocked_vehicles() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::string to_string(TrafficEventKind kind);

} // namespace airside::autonomy
