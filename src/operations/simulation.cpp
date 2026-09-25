#include "airside/operations/simulation.hpp"

#include "airside/routing/astar.hpp"

#include <algorithm>
#include <format>
#include <stdexcept>
#include <utility>

namespace airside {
namespace {

std::vector<VehicleId> vehicle_ids(const Scenario& scenario, ServiceType type) {
    std::vector<VehicleId> ids;
    for (const auto& value : scenario.vehicles) {
        if (value.capability() == type) ids.push_back(value.id());
    }
    return ids;
}

SimulationEventRecord vehicle_event(
    SimulationEventType type, const ServiceVehicle& vehicle, const Aircraft* aircraft,
    std::optional<Route> route = std::nullopt) {
    SimulationEventRecord event{type};
    event.aircraft = aircraft ? std::optional{aircraft->id()} : std::nullopt;
    event.vehicle = vehicle.id();
    event.service = vehicle.capability();
    event.route = std::move(route);
    event.aircraft_name = aircraft ? aircraft->flight_number() : std::string{};
    event.vehicle_name = vehicle.name();
    return event;
}

SimulationEventRecord aircraft_event(SimulationEventType type, const Aircraft& aircraft) {
    SimulationEventRecord event{type};
    event.aircraft = aircraft.id();
    event.gate = aircraft.gate();
    event.aircraft_name = aircraft.flight_number();
    return event;
}

}  // namespace

Simulation::Simulation(Scenario scenario, std::uint64_t seed)
    : scenario_(std::move(scenario)), seed_(seed), random_(seed),
      fuel_pool_(vehicle_ids(scenario_, ServiceType::Fueling)),
      baggage_pool_(vehicle_ids(scenario_, ServiceType::Baggage)) {
    for (const auto type : {ServiceType::Fueling, ServiceType::Baggage}) {
        if (!scenario_.service_durations.contains(type) || duration(type) <= SimTime::zero()) {
            throw std::invalid_argument("scenario requires positive service durations");
        }
    }
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
}

void Simulation::add_event_sink(ISimulationEventSink& sink) {
    if (std::ranges::find(event_sinks_, &sink) == event_sinks_.end()) event_sinks_.push_back(&sink);
}

bool Simulation::finished() const noexcept { return events_.empty(); }

bool Simulation::advance() {
    const auto event = events_.pop();
    if (!event) return false;
    if (event->timestamp < now_) throw std::logic_error("event queue moved simulation time backwards");
    now_ = event->timestamp;
    process(*event);
    return true;
}

SimulationSnapshot Simulation::snapshot() const {
    SimulationSnapshot result;
    result.simulation_time = now_;
    result.aircraft.reserve(scenario_.aircraft.size());
    for (const auto& flight : scenario_.aircraft) {
        AircraftSnapshot item{flight.id(), flight.flight_number(), flight.state(), flight.gate(),
            std::nullopt, flight.scheduled_arrival(), flight.actual_arrival(),
            flight.scheduled_departure(), flight.actual_departure(), {}};
        if (flight.state() != AircraftState::Scheduled && flight.state() != AircraftState::Departed) {
            item.logical_node = flight.gate_node();
        }
        item.services.reserve(flight.tasks().size());
        for (const auto& task : flight.tasks()) {
            item.services.push_back({task.id, task.type, task.status, task.requested_at,
                task.started_at, task.completed_at});
        }
        result.aircraft.push_back(std::move(item));
    }

    result.vehicles.reserve(scenario_.vehicles.size());
    for (const auto& service_vehicle : scenario_.vehicles) {
        ServiceVehicleSnapshot item{service_vehicle.id(), service_vehicle.name(),
            service_vehicle.capability(), service_vehicle.state(), service_vehicle.current_node(),
            service_vehicle.current_node(), service_vehicle.assigned_aircraft(), std::nullopt};
        if (service_vehicle.active_route()) {
            const auto& route = *service_vehicle.active_route();
            const auto departure = *service_vehicle.journey_departure_time();
            item.destination_node = route.nodes.back();
            VehicleJourneySnapshot journey{route.nodes.front(), route.nodes.back(), departure,
                *service_vehicle.journey_arrival_time(), route.nodes, route.edges, {}};
            auto segment_departure = departure;
            for (std::size_t index = 0; index < route.edges.size(); ++index) {
                const auto& edge = scenario_.graph.edge(route.edges[index]);
                const auto arrival = segment_departure + edge.traversal_cost;
                journey.segments.push_back({edge.id, route.nodes[index], route.nodes[index + 1],
                    segment_departure, arrival, edge.distance_m});
                segment_departure = arrival;
            }
            item.journey = std::move(journey);
        }
        result.vehicles.push_back(std::move(item));
    }

    result.gates.reserve(scenario_.gates.size());
    for (const auto& gate_value : scenario_.gates) {
        result.gates.push_back({gate_value.id, gate_value.name, gate_value.node,
            scenario_.graph.node(gate_value.node).position, gate_value.occupying_aircraft,
            gate_value.enabled && !gate_value.occupying_aircraft.has_value()});
    }
    for (const auto& node : scenario_.graph.nodes()) {
        result.road_nodes.push_back({node.id, node.name, node.position});
    }
    for (const auto& edge : scenario_.graph.edges()) {
        result.roads.push_back({edge.id, edge.from, edge.to, edge.distance_m,
            edge.traversal_cost, edge.available});
    }
    return result;
}

SimulationResult Simulation::run() {
    while (advance()) {}
    return result();
}

SimulationResult Simulation::result() const {
    if (!finished()) throw std::logic_error("simulation result requested before completion");
    return {seed_, now_, scenario_.aircraft, scenario_.vehicles, log_, event_history_,
        calculate_metrics(scenario_.aircraft, scenario_.vehicles, now_)};
}

SimTime Simulation::current_time() const noexcept { return now_; }
const std::vector<SimulationEventRecord>& Simulation::event_history() const noexcept { return event_history_; }

void Simulation::process(const Event& event) {
    switch (event.type) {
    case EventType::AircraftArrival: handle_aircraft_arrival(AircraftId{event.entity.value}); break;
    case EventType::VehicleArrivalAtAircraft: handle_vehicle_arrival(VehicleId{event.entity.value}); break;
    case EventType::ServiceCompleted: handle_service_completed(VehicleId{event.entity.value}); break;
    case EventType::VehicleArrivalAtDepot: handle_vehicle_return(VehicleId{event.entity.value}); break;
    case EventType::EdgeAvailabilityChanged:
        handle_road_event(EdgeId{event.entity.value}, event.data != 0); break;
    case EventType::AircraftDeparture: handle_departure(AircraftId{event.entity.value}); break;
    }
}

void Simulation::handle_aircraft_arrival(AircraftId id) {
    auto& flight = aircraft(id);
    auto& assigned_gate = gate(flight.gate());
    if (!assigned_gate.enabled || assigned_gate.occupying_aircraft) {
        throw std::logic_error("aircraft arrived at an unavailable gate");
    }
    assigned_gate.occupying_aircraft = id;
    const auto previous = flight.state();
    flight.arrive(now_);
    emit(aircraft_event(SimulationEventType::AircraftArrived, flight));
    emit_aircraft_state(flight, previous);
    request_service(flight, ServiceType::Fueling);
    request_service(flight, ServiceType::Baggage);
}

void Simulation::request_service(Aircraft& flight, ServiceType type) {
    const auto request = pool(type).request(flight.id());
    if (request.status == RequestStatus::Assigned) {
        dispatch(*request.vehicle, flight.id());
    } else if (request.status == RequestStatus::Queued) {
        flight.mark_task_waiting(type, now_);
        auto event = aircraft_event(SimulationEventType::ResourceWaitStarted, flight);
        event.service = type;
        emit(std::move(event));
    }
}

void Simulation::dispatch(VehicleId vehicle_id, AircraftId aircraft_id) {
    auto& service_vehicle = vehicle(vehicle_id);
    auto& flight = aircraft(aircraft_id);
    auto route = find_route(scenario_.graph, service_vehicle.current_node(), flight.gate_node());
    if (!route) throw std::runtime_error("no available route for assigned service vehicle");
    flight.assign_task(service_vehicle.capability());
    const auto assigned_route = *route;
    const auto previous = service_vehicle.state();
    service_vehicle.assign(aircraft_id, std::move(*route), now_);
    [[maybe_unused]] const auto sequence = events_.schedule(
        now_ + assigned_route.travel_time, EventType::VehicleArrivalAtAircraft,
        {EntityKind::Vehicle, vehicle_id.value()});

    emit(vehicle_event(SimulationEventType::ResourceAssigned, service_vehicle, &flight, assigned_route));
    emit(vehicle_event(SimulationEventType::VehicleAssigned, service_vehicle, &flight, assigned_route));
    emit_vehicle_state(service_vehicle, previous);
    emit(vehicle_event(SimulationEventType::VehicleDeparted, service_vehicle, &flight, assigned_route));
}

void Simulation::handle_vehicle_arrival(VehicleId id) {
    auto& service_vehicle = vehicle(id);
    auto& flight = aircraft(*service_vehicle.assigned_aircraft());
    const auto arrival_route = service_vehicle.active_route();
    const auto previous = service_vehicle.state();
    service_vehicle.arrive_at_aircraft(flight.gate_node());
    emit(vehicle_event(SimulationEventType::VehicleArrived, service_vehicle, &flight, arrival_route));
    emit_vehicle_state(service_vehicle, previous);
    const auto assigned_state = service_vehicle.state();
    flight.start_task(service_vehicle.capability(), now_);
    service_vehicle.start_service();
    emit_vehicle_state(service_vehicle, assigned_state);
    [[maybe_unused]] const auto sequence = events_.schedule(
        now_ + duration(service_vehicle.capability()), EventType::ServiceCompleted,
        {EntityKind::Vehicle, id.value()});
    emit(vehicle_event(SimulationEventType::ServiceStarted, service_vehicle, &flight));
}

void Simulation::handle_service_completed(VehicleId id) {
    auto& service_vehicle = vehicle(id);
    const auto aircraft_id = *service_vehicle.assigned_aircraft();
    auto& flight = aircraft(aircraft_id);
    const auto type = service_vehicle.capability();
    const auto previous_aircraft = flight.state();
    flight.complete_task(type, now_);
    emit(vehicle_event(SimulationEventType::ServiceCompleted, service_vehicle, &flight));
    if (flight.services_complete()) {
        emit_aircraft_state(flight, previous_aircraft);
        emit(aircraft_event(SimulationEventType::AircraftReadyForPushback, flight));
        [[maybe_unused]] const auto sequence = events_.schedule(
            std::max(now_, flight.scheduled_departure()), EventType::AircraftDeparture,
            {EntityKind::Aircraft, aircraft_id.value()});
    }

    auto return_route = find_route(scenario_.graph, flight.gate_node(), NodeId{1});
    if (!return_route) throw std::runtime_error("no available route back to vehicle depot");
    const auto route = *return_route;
    const auto previous_vehicle = service_vehicle.state();
    service_vehicle.finish_service(std::move(*return_route), duration(type), now_);
    emit_vehicle_state(service_vehicle, previous_vehicle);
    [[maybe_unused]] const auto sequence = events_.schedule(
        now_ + route.travel_time, EventType::VehicleArrivalAtDepot,
        {EntityKind::Vehicle, id.value()});
    emit(vehicle_event(SimulationEventType::VehicleDeparted, service_vehicle, &flight, route));
}

void Simulation::handle_vehicle_return(VehicleId id) {
    auto& service_vehicle = vehicle(id);
    const auto arrival_route = service_vehicle.active_route();
    const auto previous = service_vehicle.state();
    service_vehicle.arrive_at_depot();
    emit(vehicle_event(SimulationEventType::VehicleArrived, service_vehicle, nullptr, arrival_route));
    emit_vehicle_state(service_vehicle, previous);
    const auto next = pool(service_vehicle.capability()).release(id);
    if (next) dispatch(next->vehicle, next->aircraft);
}

void Simulation::handle_road_event(EdgeId id, bool available) {
    scenario_.graph.set_edge_available(id, available);
    SimulationEventRecord event{
        available ? SimulationEventType::RoadOpened : SimulationEventType::RoadClosed};
    event.edge = id;
    emit(std::move(event));
}

void Simulation::handle_departure(AircraftId id) {
    auto& flight = aircraft(id);
    const auto previous = flight.state();
    flight.depart(now_);
    auto& assigned_gate = gate(flight.gate());
    if (assigned_gate.occupying_aircraft != id) {
        throw std::logic_error("departing aircraft does not occupy its assigned gate");
    }
    assigned_gate.occupying_aircraft.reset();
    emit_aircraft_state(flight, previous);
    emit(aircraft_event(SimulationEventType::AircraftDeparted, flight));
}

void Simulation::emit(SimulationEventRecord event) {
    event.sequence = next_event_record_sequence_++;
    event.timestamp = now_;
    event_history_.push_back(std::move(event));
    log_.push_back(std::format("{}  {}", format_sim_time(now_), format_event(event_history_.back())));
    for (auto* sink : event_sinks_) sink->on_event(event_history_.back());
}

void Simulation::emit_aircraft_state(Aircraft& flight, AircraftState previous) {
    auto event = aircraft_event(SimulationEventType::AircraftStateChanged, flight);
    event.previous_aircraft_state = previous;
    event.aircraft_state = flight.state();
    emit(std::move(event));
}

void Simulation::emit_vehicle_state(ServiceVehicle& service_vehicle, VehicleState previous) {
    auto event = vehicle_event(SimulationEventType::VehicleStateChanged, service_vehicle, nullptr);
    event.aircraft = service_vehicle.assigned_aircraft();
    event.previous_vehicle_state = previous;
    event.vehicle_state = service_vehicle.state();
    emit(std::move(event));
}

Aircraft& Simulation::aircraft(AircraftId id) {
    const auto found = std::ranges::find_if(scenario_.aircraft,
        [&](const auto& value) { return value.id() == id; });
    if (found == scenario_.aircraft.end()) throw std::out_of_range("unknown aircraft ID");
    return *found;
}

ServiceVehicle& Simulation::vehicle(VehicleId id) {
    const auto found = std::ranges::find_if(scenario_.vehicles,
        [&](const auto& value) { return value.id() == id; });
    if (found == scenario_.vehicles.end()) throw std::out_of_range("unknown vehicle ID");
    return *found;
}

ResourcePool& Simulation::pool(ServiceType type) {
    return type == ServiceType::Fueling ? fuel_pool_ : baggage_pool_;
}

SimTime Simulation::duration(ServiceType type) const { return scenario_.service_durations.at(type); }

Gate& Simulation::gate(GateId id) {
    return const_cast<Gate&>(std::as_const(*this).gate(id));
}

const Gate& Simulation::gate(GateId id) const {
    const auto found = std::ranges::find_if(scenario_.gates,
        [&](const auto& value) { return value.id == id; });
    if (found == scenario_.gates.end()) throw std::out_of_range("unknown gate ID");
    return *found;
}

std::string format_sim_time(SimTime time) {
    const auto total = time.count();
    return std::format("{:02}:{:02}:{:02}", total / 3600, (total % 3600) / 60, total % 60);
}

}  // namespace airside
