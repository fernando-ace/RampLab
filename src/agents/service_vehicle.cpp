#include "airside/agents/service_vehicle.hpp"

#include <stdexcept>
#include <utility>

namespace airside {

ServiceVehicle::ServiceVehicle(
    VehicleId id,
    std::string name,
    ServiceType capability,
    NodeId depot_node)
    : id_(id),
      name_(std::move(name)),
      capability_(capability),
      depot_node_(depot_node),
      current_node_(depot_node) {
    if (name_.empty()) {
        throw std::invalid_argument("vehicle name cannot be empty");
    }
}

VehicleId ServiceVehicle::id() const noexcept { return id_; }
const std::string& ServiceVehicle::name() const noexcept { return name_; }
ServiceType ServiceVehicle::capability() const noexcept { return capability_; }
VehicleState ServiceVehicle::state() const noexcept { return state_; }
NodeId ServiceVehicle::current_node() const noexcept { return current_node_; }
std::optional<AircraftId> ServiceVehicle::assigned_aircraft() const noexcept { return assigned_aircraft_; }
const std::optional<Route>& ServiceVehicle::active_route() const noexcept { return active_route_; }
const std::optional<Route>& ServiceVehicle::last_route() const noexcept { return last_route_; }
SimTime ServiceVehicle::busy_time() const noexcept { return busy_time_; }

void ServiceVehicle::assign(AircraftId aircraft, Route route) {
    if (state_ != VehicleState::Idle || route.nodes.empty() || route.nodes.front() != current_node_) {
        throw std::logic_error("vehicle cannot accept this assignment");
    }
    state_ = VehicleState::Assigned;
    assigned_aircraft_ = aircraft;
    active_route_ = std::move(route);
    busy_time_ += active_route_->travel_time;
    state_ = VehicleState::TravelingToAircraft;
}

void ServiceVehicle::arrive_at_aircraft(NodeId gate_node) {
    if (state_ != VehicleState::TravelingToAircraft || !active_route_ ||
        active_route_->nodes.back() != gate_node) {
        throw std::logic_error("vehicle is not traveling to this aircraft location");
    }
    current_node_ = gate_node;
    last_route_ = active_route_;
    active_route_.reset();
    state_ = VehicleState::Assigned;
}

void ServiceVehicle::start_service() {
    if (state_ != VehicleState::Assigned || !assigned_aircraft_) {
        throw std::logic_error("vehicle has no service assignment to start");
    }
    state_ = VehicleState::Servicing;
}

void ServiceVehicle::finish_service(Route return_route, SimTime service_duration) {
    if (state_ != VehicleState::Servicing || return_route.nodes.empty() ||
        return_route.nodes.front() != current_node_ || return_route.nodes.back() != depot_node_) {
        throw std::logic_error("vehicle cannot finish service with this return route");
    }
    busy_time_ += service_duration + return_route.travel_time;
    active_route_ = std::move(return_route);
    assigned_aircraft_.reset();
    state_ = VehicleState::ReturningToDepot;
}

void ServiceVehicle::arrive_at_depot() {
    if (state_ != VehicleState::ReturningToDepot || !active_route_ ||
        active_route_->nodes.back() != depot_node_) {
        throw std::logic_error("vehicle is not returning to its depot");
    }
    current_node_ = depot_node_;
    last_route_ = active_route_;
    active_route_.reset();
    state_ = VehicleState::Idle;
}

}  // namespace airside
