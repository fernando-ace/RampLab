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
    ResourceWaitStarted, ResourceAssigned,
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
    std::optional<AircraftState> previous_aircraft_state;
    std::optional<AircraftState> aircraft_state;
    std::optional<VehicleState> previous_vehicle_state;
    std::optional<VehicleState> vehicle_state;
    std::optional<Route> route;
    std::string aircraft_name;
    std::string vehicle_name;

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
[[nodiscard]] std::string format_event(const SimulationEventRecord& event);

}  // namespace airside
