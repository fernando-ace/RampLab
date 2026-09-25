#pragma once

#include "airside/agents/aircraft.hpp"
#include "airside/routing/astar.hpp"

#include <optional>
#include <string>

namespace airside {

enum class VehicleState { Idle, Assigned, TravelingToAircraft, Servicing, ReturningToDepot };

class ServiceVehicle {
public:
    ServiceVehicle(VehicleId id, std::string name, ServiceType capability, NodeId depot_node);

    [[nodiscard]] VehicleId id() const noexcept;
    [[nodiscard]] const std::string& name() const noexcept;
    [[nodiscard]] ServiceType capability() const noexcept;
    [[nodiscard]] VehicleState state() const noexcept;
    [[nodiscard]] NodeId current_node() const noexcept;
    [[nodiscard]] NodeId depot_node() const noexcept;
    [[nodiscard]] std::optional<AircraftId> assigned_aircraft() const noexcept;
    [[nodiscard]] const std::optional<Route>& active_route() const noexcept;
    [[nodiscard]] const std::optional<Route>& last_route() const noexcept;
    [[nodiscard]] SimTime busy_time() const noexcept;
    [[nodiscard]] std::optional<SimTime> journey_departure_time() const noexcept;
    [[nodiscard]] std::optional<SimTime> journey_arrival_time() const noexcept;

    void assign(AircraftId aircraft, Route route, SimTime departure_time = SimTime::zero());
    void arrive_at_aircraft(NodeId gate_node);
    void start_service();
    void finish_service(
        Route return_route,
        SimTime service_duration,
        SimTime departure_time = SimTime::zero());
    void arrive_at_depot();

private:
    VehicleId id_;
    std::string name_;
    ServiceType capability_;
    VehicleState state_{VehicleState::Idle};
    NodeId depot_node_;
    NodeId current_node_;
    std::optional<AircraftId> assigned_aircraft_;
    std::optional<Route> active_route_;
    std::optional<Route> last_route_;
    SimTime busy_time_{};
    std::optional<SimTime> journey_departure_time_;
    std::optional<SimTime> journey_arrival_time_;
};

}  // namespace airside
