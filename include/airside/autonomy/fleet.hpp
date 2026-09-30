#pragma once

#include "airside/autonomy/simulation.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace airside::autonomy {

struct VehicleId {
    std::string value;
    auto operator<=>(const VehicleId&) const = default;
};

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
};

[[nodiscard]] FleetScenario load_fleet_scenario(const std::filesystem::path& path);

enum class TrafficEventKind { Request, Granted, Deferred, Waiting, EnteredConflict, ReleasedConflict, Collision, DeadlockDetected, DeadlockRecovery };
struct ReservationRequest {
    VehicleId vehicle;
    double time_s{};
    int priority{};
};

// Deterministic exclusive-resource arbiter with timestamp, priority, and ID ordering.
class TrafficReservationTable {
public:
    [[nodiscard]] bool request(std::string resource, ReservationRequest request);
    [[nodiscard]] std::optional<VehicleId> request_batch(
        std::string resource, std::vector<ReservationRequest> requests);
    [[nodiscard]] bool release(const std::string& resource, const VehicleId& vehicle);
    [[nodiscard]] std::optional<VehicleId> owner(const std::string& resource) const;

private:
    struct Entry { VehicleId owner; ReservationRequest request; };
    std::map<std::string, Entry> held_;
    std::map<std::string, std::vector<ReservationRequest>> waiting_;
};

struct TrafficEvent {
    double time_s{};
    TrafficEventKind kind{};
    VehicleId vehicle;
    VehicleId other;
    std::string resource;
};

struct FleetVehicleResult {
    VehicleId id;
    std::string start_node;
    std::string goal_node;
    MissionMetrics metrics;
    VehicleState final_state;
};
struct FleetVehicleSnapshot {
    VehicleId id;
    std::string goal_node;
    bool waiting{};
    AutonomySnapshot autonomy;
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
    double throughput_per_simulated_hour{};
    std::vector<FleetVehicleResult> vehicles;
    std::vector<TrafficEvent> events;
    std::uint64_t deterministic_digest{};
};

class FleetSimulation {
public:
    FleetSimulation(AutonomyScenario base, std::vector<FleetMission> missions, std::uint64_t seed);
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

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::string to_string(TrafficEventKind kind);

} // namespace airside::autonomy
