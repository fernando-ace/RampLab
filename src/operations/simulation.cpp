#include "airside/operations/simulation.hpp"

#include "airside/routing/astar.hpp"

#include <algorithm>
#include <format>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace airside {
namespace {

std::vector<VehicleId> vehicle_ids(const Scenario& scenario, ServiceType type) {
    std::vector<VehicleId> ids;
    for (const auto& vehicle : scenario.vehicles) {
        if (vehicle.capability() == type) {
            ids.push_back(vehicle.id());
        }
    }
    return ids;
}

std::string service_name(ServiceType type) {
    return type == ServiceType::Fueling ? "fuel" : "baggage";
}

std::string route_text(const Route& route) {
    std::string text;
    for (std::size_t index = 0; index < route.nodes.size(); ++index) {
        if (index != 0) {
            text += "->";
        }
        text += std::to_string(route.nodes[index].value());
    }
    return text;
}

}  // namespace

Simulation::Simulation(Scenario scenario, std::uint64_t seed, bool verbose)
    : scenario_(std::move(scenario)),
      seed_(seed),
      verbose_(verbose),
      random_(seed),
      fuel_pool_(vehicle_ids(scenario_, ServiceType::Fueling)),
      baggage_pool_(vehicle_ids(scenario_, ServiceType::Baggage)) {
    for (const auto type : {ServiceType::Fueling, ServiceType::Baggage}) {
        if (!scenario_.service_durations.contains(type) || duration(type) <= SimTime::zero()) {
            throw std::invalid_argument("scenario requires positive service durations");
        }
    }
}

SimulationResult Simulation::run() {
    for (const auto& flight : scenario_.aircraft) {
        [[maybe_unused]] const auto sequence = events_.schedule(
            flight.scheduled_arrival(), EventType::AircraftArrival,
            {EntityKind::Aircraft, flight.id().value()});
    }
    for (const auto& road_event : scenario_.road_events) {
        [[maybe_unused]] const auto sequence = events_.schedule(
            road_event.time, EventType::EdgeAvailabilityChanged,
            {EntityKind::Edge, road_event.edge.value()}, road_event.available ? 1 : 0);
    }

    while (const auto event = events_.pop()) {
        if (event->timestamp < now_) {
            throw std::logic_error("event queue moved simulation time backwards");
        }
        now_ = event->timestamp;
        process(*event);
    }

    auto metrics = calculate_metrics(scenario_.aircraft, scenario_.vehicles, now_);
    return {seed_, now_, scenario_.aircraft, scenario_.vehicles, log_, std::move(metrics)};
}

void Simulation::process(const Event& event) {
    switch (event.type) {
    case EventType::AircraftArrival: handle_aircraft_arrival(AircraftId{event.entity.value}); break;
    case EventType::VehicleArrivalAtAircraft: handle_vehicle_arrival(VehicleId{event.entity.value}); break;
    case EventType::ServiceCompleted: handle_service_completed(VehicleId{event.entity.value}); break;
    case EventType::VehicleArrivalAtDepot: handle_vehicle_return(VehicleId{event.entity.value}); break;
    case EventType::EdgeAvailabilityChanged:
        handle_road_event(EdgeId{event.entity.value}, event.data != 0);
        break;
    case EventType::AircraftDeparture: handle_departure(AircraftId{event.entity.value}); break;
    }
}

void Simulation::handle_aircraft_arrival(AircraftId id) {
    auto& flight = aircraft(id);
    flight.arrive(now_);
    record(std::format("{} arrived at Gate A{}", flight.flight_number(), flight.gate().value()));
    request_service(flight, ServiceType::Fueling);
    request_service(flight, ServiceType::Baggage);
}

void Simulation::request_service(Aircraft& flight, ServiceType type) {
    auto result = pool(type).request(flight.id());
    if (result.status == RequestStatus::Assigned) {
        dispatch(*result.vehicle, flight.id());
        return;
    }
    if (result.status == RequestStatus::Queued) {
        flight.mark_task_waiting(type, now_);
        record(std::format("{} waiting for {} resource", flight.flight_number(), service_name(type)));
    }
}

void Simulation::dispatch(VehicleId vehicle_id, AircraftId aircraft_id) {
    auto& service_vehicle = vehicle(vehicle_id);
    auto& flight = aircraft(aircraft_id);
    auto route = find_route(scenario_.graph, service_vehicle.current_node(), flight.gate_node());
    if (!route) {
        throw std::runtime_error("no available route for assigned service vehicle");
    }
    flight.assign_task(service_vehicle.capability());
    const auto route_description = route_text(*route);
    const auto travel_time = route->travel_time;
    service_vehicle.assign(aircraft_id, std::move(*route));
    [[maybe_unused]] const auto sequence = events_.schedule(
        now_ + travel_time, EventType::VehicleArrivalAtAircraft,
        {EntityKind::Vehicle, vehicle_id.value()});
    record(std::format(
        "{} assigned to {} (route {}, {} sec)", service_vehicle.name(),
        flight.flight_number(), route_description, travel_time.count()));
}

void Simulation::handle_vehicle_arrival(VehicleId id) {
    auto& service_vehicle = vehicle(id);
    auto& flight = aircraft(*service_vehicle.assigned_aircraft());
    service_vehicle.arrive_at_aircraft(flight.gate_node());
    flight.start_task(service_vehicle.capability(), now_);
    service_vehicle.start_service();
    [[maybe_unused]] const auto sequence = events_.schedule(
        now_ + duration(service_vehicle.capability()), EventType::ServiceCompleted,
        {EntityKind::Vehicle, id.value()});
    record(std::format(
        "{} arrived at {}; {} service started", service_vehicle.name(),
        flight.flight_number(), service_name(service_vehicle.capability())));
}

void Simulation::handle_service_completed(VehicleId id) {
    auto& service_vehicle = vehicle(id);
    const auto aircraft_id = *service_vehicle.assigned_aircraft();
    auto& flight = aircraft(aircraft_id);
    const auto type = service_vehicle.capability();
    flight.complete_task(type, now_);
    record(std::format("{} service completed for {}", service_name(type), flight.flight_number()));

    if (flight.services_complete()) {
        const auto departure = std::max(now_, flight.scheduled_departure());
        [[maybe_unused]] const auto sequence = events_.schedule(
            departure, EventType::AircraftDeparture, {EntityKind::Aircraft, aircraft_id.value()});
        record(std::format("{} ready for pushback", flight.flight_number()));
    }

    auto return_route = find_route(scenario_.graph, flight.gate_node(), NodeId{1});
    if (!return_route) {
        throw std::runtime_error("no available route back to vehicle depot");
    }
    const auto return_time = return_route->travel_time;
    service_vehicle.finish_service(std::move(*return_route), duration(type));
    [[maybe_unused]] const auto sequence = events_.schedule(
        now_ + return_time, EventType::VehicleArrivalAtDepot,
        {EntityKind::Vehicle, id.value()});
}

void Simulation::handle_vehicle_return(VehicleId id) {
    auto& service_vehicle = vehicle(id);
    service_vehicle.arrive_at_depot();
    record(std::format("{} returned to depot", service_vehicle.name()));
    const auto next = pool(service_vehicle.capability()).release(id);
    if (next) {
        dispatch(next->vehicle, next->aircraft);
    }
}

void Simulation::handle_road_event(EdgeId id, bool available) {
    scenario_.graph.set_edge_available(id, available);
    record(std::format("road edge {} {}", id.value(), available ? "opened" : "closed"));
}

void Simulation::handle_departure(AircraftId id) {
    auto& flight = aircraft(id);
    flight.depart(now_);
    record(std::format("{} departed", flight.flight_number()));
}

void Simulation::record(std::string message) {
    auto entry = std::format("{}  {}", format_sim_time(now_), std::move(message));
    log_.push_back(entry);
    if (verbose_) {
        std::cout << entry << '\n';
    }
}

Aircraft& Simulation::aircraft(AircraftId id) {
    const auto found = std::ranges::find_if(
        scenario_.aircraft, [&](const auto& value) { return value.id() == id; });
    if (found == scenario_.aircraft.end()) {
        throw std::out_of_range("unknown aircraft ID");
    }
    return *found;
}

ServiceVehicle& Simulation::vehicle(VehicleId id) {
    const auto found = std::ranges::find_if(
        scenario_.vehicles, [&](const auto& value) { return value.id() == id; });
    if (found == scenario_.vehicles.end()) {
        throw std::out_of_range("unknown vehicle ID");
    }
    return *found;
}

ResourcePool& Simulation::pool(ServiceType type) {
    return type == ServiceType::Fueling ? fuel_pool_ : baggage_pool_;
}

SimTime Simulation::duration(ServiceType type) const {
    return scenario_.service_durations.at(type);
}

std::string format_sim_time(SimTime time) {
    const auto total = time.count();
    const auto hours = total / 3600;
    const auto minutes = (total % 3600) / 60;
    const auto seconds = total % 60;
    return std::format("{:02}:{:02}:{:02}", hours, minutes, seconds);
}

}  // namespace airside
