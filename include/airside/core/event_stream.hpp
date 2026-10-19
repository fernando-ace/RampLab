#pragma once

#include "airside/agents/aircraft.hpp"
#include "airside/agents/service_vehicle.hpp"
#include "airside/routing/astar.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace airside {

enum class SimulationEventType {
    AircraftArrived, AircraftStateChanged, VehicleAssigned, VehicleDeparted,
    VehicleArrived, VehicleStateChanged, ServiceStarted, ServiceCompleted,
    AircraftReadyForPushback, AircraftDeparted, RoadClosed, RoadOpened,
    ResourceWaitStarted, ResourceAssigned, TurnaroundCreated, TurnaroundTaskReady,
    TurnaroundTaskDispatched, TurnaroundTaskStarted, TurnaroundTaskCompleted,
    TurnaroundTaskReassigned, TurnaroundCriticalPathChanged, TurnaroundPredictedLate,
    TurnaroundReadyForDeparture, TurnaroundDisruptionDetected, TurnaroundTaskFailed, TurnaroundFailed,
    TurnaroundVehicleUnavailable,
    SurfacePushbackRequested, SurfacePushbackStarted, SurfacePushbackCompleted,
    SurfaceTaxiRouteAssigned, SurfaceWaitingForTraffic, SurfaceRerouted,
    SurfaceRunwayQueueEntered, SurfaceRunwayClearance, SurfaceSafeFailure,
    SurfaceRouteInvalidated, SurfaceReservationAcquired, SurfaceReservationReleased,
    SurfaceWaitingForPushback,
    RunwayRequest, RunwayGrant, RunwayOccupied, RunwayReleased,
    ArrivalRunwayExit, ArrivalTaxiInStarted, ArrivalAtGate,
    GateWaitStarted, GateAssigned, GateAvailabilityChanged, AircraftLanded, TurnaroundStarted, PushbackTaxiOutStarted,
    OperatorIntervention, AgentDecision,
};

struct SimulationEventRecord {
    std::uint64_t sequence{0};
    SimTime timestamp{};
    SimulationEventType type{SimulationEventType::AircraftArrived};
    std::optional<AircraftId> aircraft;
    std::optional<VehicleId> vehicle;
    std::optional<GateId> gate;
    std::optional<EdgeId> edge;
    std::optional<ServiceType> service;
    std::optional<TaskId> task;
    std::string turnaround_id;
    std::optional<SimTime> estimated_ready_time;
    std::optional<SimTime> schedule_slack;
    std::optional<SimTime> task_duration;
    std::optional<AircraftState> previous_aircraft_state;
    std::optional<AircraftState> aircraft_state;
    std::optional<VehicleState> previous_vehicle_state;
    std::optional<VehicleState> vehicle_state;
    std::optional<Route> route;
    std::optional<Route> previous_route;
    // Position/orientation sampled from the authoritative simulation snapshot
    // at this event time. Empty means the entity has no defined surface pose.
    std::optional<Vec2> position_m;
    std::optional<double> heading_rad;
    std::optional<Vec2> related_position_m;
    std::optional<double> related_heading_rad;
    std::string aircraft_name;
    std::string vehicle_name;
    std::string agent_id;
    std::string agent_event_id;
    std::string agent_type;
    std::string policy_id;
    std::string objective_id;
    std::string decision;
    std::string reason_code;
    std::string observation_summary;
    std::string decision_result;
    std::string detail;

    SimulationEventRecord() = default;
    explicit SimulationEventRecord(SimulationEventType event_type) noexcept : type(event_type) {}

    auto operator<=>(const SimulationEventRecord&) const = default;
};

class ISimulationEventSink {
public:
    virtual ~ISimulationEventSink() = default;
    virtual void on_event(const SimulationEventRecord& event) noexcept = 0;
};

[[nodiscard]] std::string_view to_string(SimulationEventType type) noexcept;
[[nodiscard]] std::string_view to_string(AircraftState state) noexcept;
[[nodiscard]] std::string_view to_string(VehicleState state) noexcept;
[[nodiscard]] std::string_view to_string(ServiceType type) noexcept;
[[nodiscard]] std::string_view to_string(TaskStatus state) noexcept;
[[nodiscard]] std::string format_event(const SimulationEventRecord& event);

}  // namespace airside
