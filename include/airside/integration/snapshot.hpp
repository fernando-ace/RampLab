#pragma once

#include "airside/agents/aircraft.hpp"
#include "airside/agents/service_vehicle.hpp"
#include "airside/world/airport_graph.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace airside {

struct SnapshotVersion {
    std::uint32_t value{1};
    constexpr auto operator<=>(const SnapshotVersion&) const = default;
};

inline constexpr SnapshotVersion kSnapshotSchemaVersion{1};

struct ServiceTaskSnapshot {
    TaskId id;
    ServiceType type;
    TaskStatus status;
    std::optional<SimTime> requested_at;
    std::optional<SimTime> started_at;
    std::optional<SimTime> completed_at;
    std::vector<TaskId> prerequisites;
    SimTime earliest_start{};
    SimTime waiting_time{};
    std::string required_resource;
    std::optional<VehicleId> assigned_vehicle;
    std::string assigned_resource;
    auto operator<=>(const ServiceTaskSnapshot&) const = default;
};

struct TurnaroundSnapshot {
    std::string turnaround_id;
    AircraftId aircraft;
    GateId gate;
    TurnaroundState state{TurnaroundState::Scheduled};
    SimTime scheduled_arrival{};
    std::optional<SimTime> actual_arrival;
    SimTime scheduled_departure{};
    SimTime target_off_block{};
    std::optional<SimTime> completion_time;
    std::optional<SimTime> departure_delay;
    SimTime estimated_ready_time{};
    SimTime schedule_slack{};
    bool predicted_late{false};
    std::vector<TaskId> critical_path_tasks;
    std::vector<ServiceTaskSnapshot> tasks;
    auto operator<=>(const TurnaroundSnapshot&) const = default;
};

struct AircraftSnapshot {
    AircraftId id;
    std::string flight_number;
    AircraftState state;
    GateId assigned_gate;
    std::optional<NodeId> logical_node;
    SimTime scheduled_arrival;
    std::optional<SimTime> actual_arrival;
    SimTime scheduled_departure;
    std::optional<SimTime> actual_departure;
    std::vector<ServiceTaskSnapshot> services;
    auto operator<=>(const AircraftSnapshot&) const = default;
};

struct RouteSegmentSnapshot {
    EdgeId edge;
    NodeId from;
    NodeId to;
    SimTime departure_time;
    SimTime expected_arrival_time;
    double distance_m{0.0};
    auto operator<=>(const RouteSegmentSnapshot&) const = default;
};

struct VehicleJourneySnapshot {
    NodeId origin;
    NodeId destination;
    SimTime departure_time;
    SimTime expected_arrival_time;
    std::vector<NodeId> route_nodes;
    std::vector<EdgeId> route_edges;
    std::vector<RouteSegmentSnapshot> segments;
    auto operator<=>(const VehicleJourneySnapshot&) const = default;
};

struct ServiceVehicleSnapshot {
    VehicleId id;
    std::string name;
    ServiceType type;
    VehicleState state;
    NodeId current_node;
    NodeId destination_node;
    std::optional<AircraftId> assigned_aircraft;
    std::optional<VehicleJourneySnapshot> journey;
    auto operator<=>(const ServiceVehicleSnapshot&) const = default;
};

struct GateSnapshot {
    GateId id;
    std::string name;
    NodeId node;
    Vec2 position_m;
    std::optional<AircraftId> occupying_aircraft;
    bool available{true};
    auto operator<=>(const GateSnapshot&) const = default;
};

struct RoadNodeSnapshot {
    NodeId id;
    std::string name;
    Vec2 position_m;
    auto operator<=>(const RoadNodeSnapshot&) const = default;
};

struct RoadEdgeSnapshot {
    EdgeId id;
    NodeId source;
    NodeId destination;
    double distance_m{0.0};
    SimTime traversal_time{};
    bool enabled{true};
    auto operator<=>(const RoadEdgeSnapshot&) const = default;
};

struct SimulationSnapshot {
    SnapshotVersion version{kSnapshotSchemaVersion};
    SimTime simulation_time{};
    std::vector<AircraftSnapshot> aircraft;
    std::vector<ServiceVehicleSnapshot> vehicles;
    std::vector<GateSnapshot> gates;
    std::vector<RoadNodeSnapshot> road_nodes;
    std::vector<RoadEdgeSnapshot> roads;
    std::vector<TurnaroundSnapshot> turnarounds;
    auto operator<=>(const SimulationSnapshot&) const = default;
};

}  // namespace airside
