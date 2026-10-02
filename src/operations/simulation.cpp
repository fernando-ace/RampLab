#include "airside/operations/simulation.hpp"

#include "airside/autonomy/fleet.hpp"
#include "airside/routing/astar.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <functional>
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

bool uses_mobile_fleet(ServiceType type) {
    return type == ServiceType::Fueling || type == ServiceType::Baggage || type == ServiceType::BaggageLoad;
}

}  // namespace

Simulation::Simulation(Scenario scenario, std::uint64_t seed, SimulationHistoryPolicy history_policy)
    : scenario_(std::move(scenario)), seed_(seed), random_(seed),
      fuel_pool_(vehicle_ids(scenario_, ServiceType::Fueling)),
      baggage_pool_(vehicle_ids(scenario_, ServiceType::Baggage)), history_policy_(history_policy) {
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
    for (const auto& disruption : scenario_.task_duration_disruptions) {
        [[maybe_unused]] const auto sequence = events_.schedule(disruption.time, EventType::TaskDurationChanged,
            {EntityKind::Task, disruption.task.value()}, disruption.duration.count());
    }
    for (const auto& outage : scenario_.vehicle_outages) {
        [[maybe_unused]] const auto sequence = events_.schedule(outage.time, EventType::VehicleOutage,
            {EntityKind::Vehicle, outage.vehicle.value()});
    }
    if (scenario_.turnaround_orchestration && !scenario_.vehicles.empty()) {
        autonomy::FleetScenario fleet_scenario;
        fleet_scenario.name = scenario_.name + "_turnaround_fleet";
        fleet_scenario.vehicle_scenario.name = fleet_scenario.name;
        fleet_scenario.vehicle_scenario.airport.graph = scenario_.graph;
        fleet_scenario.vehicle_scenario.timestep_s = 0.02;
        fleet_scenario.vehicle_scenario.timeout_s = 1800.0;
        fleet_scenario.vehicle_scenario.limits.maximum_speed_mps = 10.0;
        fleet_scenario.vehicle_scenario.limits.radius_m = 0.75;
        fleet_scenario.vehicle_scenario.safety_stop_range_m = 5.0;
        fleet_scenario.dispatch_aging_interval_s = 30.0;
        for (const auto& change : scenario_.road_events) {
            fleet_scenario.road_events.push_back({change.time, change.edge, change.available});
        }
        for (const auto& service_vehicle : scenario_.vehicles) {
            const auto& depot = scenario_.graph.node(service_vehicle.depot_node());
            const auto id = autonomy::VehicleId{std::to_string(service_vehicle.id().value())};
            const auto capability = service_vehicle.capability() == ServiceType::Fueling
                ? "fuel_truck" : "baggage_vehicle";
            fleet_scenario.dispatch_fleet.push_back({id, {capability}, depot.name,
                autonomy::DispatchVehicleState::Idle, std::nullopt, {}});
        }
        autonomy_fleet_ = std::make_unique<autonomy::FleetSimulation>(std::move(fleet_scenario), seed_);
    }
}

Simulation::~Simulation() = default;

void Simulation::add_event_sink(ISimulationEventSink& sink) {
    if (std::ranges::find(event_sinks_, &sink) == event_sinks_.end()) event_sinks_.push_back(&sink);
}

bool Simulation::finished() const noexcept { return events_.empty(); }

std::optional<SimTime> Simulation::next_event_time() const noexcept {
    const auto* event = events_.peek();
    return event == nullptr ? std::nullopt : std::optional<SimTime>{event->timestamp};
}

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
    if (autonomy_fleet_) {
        const auto fleet_tasks = autonomy_fleet_->service_requests();
        for (const auto& fleet_vehicle : autonomy_fleet_->snapshots()) {
            const auto numeric_id = static_cast<std::uint32_t>(std::stoul(fleet_vehicle.id.value));
            const auto item = std::ranges::find(result.vehicles, VehicleId{numeric_id}, &ServiceVehicleSnapshot::id);
            if (item == result.vehicles.end()) continue;
            const auto position = fleet_vehicle.autonomy.ground_truth.position;
            item->observed_position_m = position;
            item->observed_heading_rad = fleet_vehicle.autonomy.ground_truth.heading_rad;
            item->fleet_status = fleet_vehicle.dispatch_state == autonomy::DispatchVehicleState::Unavailable
                ? "Unavailable" : fleet_vehicle.dispatch_state == autonomy::DispatchVehicleState::Recovering
                ? "Recovering" : fleet_vehicle.current_request ? "Busy" : "Idle";
            const auto nearest = std::ranges::min_element(scenario_.graph.nodes(), [&](const auto& a, const auto& b) {
                const auto distance = [&](const auto& node) {
                    return std::hypot(position.x_m - node.position.x_m, position.y_m - node.position.y_m);
                };
                const auto da = distance(a), db = distance(b);
                return da == db ? a.id < b.id : da < db;
            });
            if (nearest != scenario_.graph.nodes().end()) item->current_node = nearest->id;
            if (!fleet_vehicle.goal_node.empty()) {
                const auto destination = std::ranges::find(scenario_.graph.nodes(), fleet_vehicle.goal_node,
                    &AirportNode::name);
                if (destination != scenario_.graph.nodes().end()) item->destination_node = destination->id;
            }
            if (fleet_vehicle.current_request) {
                const auto request = fleet_task_requests_.find(fleet_vehicle.current_request->value);
                if (request != fleet_task_requests_.end()) {
                    item->assigned_aircraft = request->second.first;
                    const auto fleet_task = std::ranges::find(fleet_tasks,
                        *fleet_vehicle.current_request, [](const auto& candidate) { return candidate.request.id; });
                    if (fleet_task != fleet_tasks.end()) {
                        item->fleet_status = fleet_task->state == autonomy::ServiceTaskState::EnRoute
                            ? "TravelingToAircraft" : fleet_task->state == autonomy::ServiceTaskState::Servicing
                            ? "Servicing" : "Assigned";
                    }
                }
            }
        }
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
    for (const auto& flight : scenario_.aircraft) {
        if (!scenario_.turnaround_orchestration) break;
        TurnaroundSnapshot turnaround;
        turnaround.turnaround_id = flight.turnaround_id();
        turnaround.aircraft = flight.id();
        turnaround.gate = flight.gate();
        turnaround.state = flight.turnaround_state();
        turnaround.scheduled_arrival = flight.scheduled_arrival();
        turnaround.actual_arrival = flight.actual_arrival();
        turnaround.scheduled_departure = flight.scheduled_departure();
        turnaround.target_off_block = flight.target_off_block();
        turnaround.completion_time = flight.ready_at();
        turnaround.departure_delay = flight.departure_delay();
        turnaround.failure_reason = flight.failure_reason();
        if (const auto found = estimated_ready_times_.find(flight.id()); found != estimated_ready_times_.end()) {
            turnaround.estimated_ready_time = found->second;
        } else turnaround.estimated_ready_time = flight.actual_arrival().value_or(flight.scheduled_arrival());
        if (const auto found = schedule_slacks_.find(flight.id()); found != schedule_slacks_.end()) turnaround.schedule_slack = found->second;
        if (const auto found = predicted_late_.find(flight.id()); found != predicted_late_.end()) turnaround.predicted_late = found->second;
        if (const auto found = critical_paths_.find(flight.id()); found != critical_paths_.end()) turnaround.critical_path_tasks = found->second;
        for (const auto& task : flight.tasks()) {
            SimTime wait{};
            if (task.requested_at && task.started_at) wait = *task.started_at - *task.requested_at;
            else if (task.requested_at) wait = now_ - *task.requested_at;
            turnaround.tasks.push_back({task.id, task.type, task.status, task.requested_at, task.started_at,
                task.completed_at, task.prerequisites, task.earliest_start, wait,
                task.required_resource, task.assigned_vehicle, task.assigned_resource,
                task.latest_desirable_completion, task.reassignments});
        }
        result.turnarounds.push_back(std::move(turnaround));
    }
    return result;
}

SimulationResult Simulation::run() {
    while (advance()) {}
    return result();
}

SimulationResult Simulation::result() const {
    if (!finished()) throw std::logic_error("simulation result requested before completion");
    std::vector<Aircraft> completed_aircraft;
    if (scenario_.turnaround_orchestration) {
        std::ranges::copy_if(scenario_.aircraft, std::back_inserter(completed_aircraft),
            [](const Aircraft& flight) { return flight.actual_departure().has_value(); });
    }
    auto metrics = calculate_metrics(scenario_.turnaround_orchestration ? completed_aircraft : scenario_.aircraft,
        scenario_.vehicles, now_);
    if (scenario_.turnaround_orchestration) {
        metrics.total_turnarounds = scenario_.aircraft.size();
        std::int64_t total_delay = 0;
        std::int64_t total_turnaround_duration = 0;
        std::unordered_map<ServiceType, std::int64_t> service_seconds;
        std::unordered_map<ServiceType, std::size_t> service_counts;
        for (const auto& flight : scenario_.aircraft) {
            const auto found = std::ranges::find(metrics.aircraft, flight.id(), &AircraftMetrics::id);
            if (found == metrics.aircraft.end()) continue;
            found->turnaround_id = flight.turnaround_id();
            found->target_off_block = flight.target_off_block();
            if (flight.actual_arrival() && flight.ready_at()) {
                found->turnaround = *flight.ready_at() - *flight.actual_arrival();
                found->actual_completion_time = flight.ready_at();
                total_turnaround_duration += found->turnaround.count();
                metrics.maximum_turnaround_seconds = std::max(metrics.maximum_turnaround_seconds, found->turnaround.count());
                ++metrics.completed_turnarounds;
            }
            if (const auto ready = estimated_ready_times_.find(flight.id()); ready != estimated_ready_times_.end()) {
                found->estimated_ready_time = ready->second;
            }
            if (const auto slack = schedule_slacks_.find(flight.id()); slack != schedule_slacks_.end()) {
                found->schedule_slack = slack->second;
            }
            if (const auto path = critical_paths_.find(flight.id()); path != critical_paths_.end()) {
                found->critical_path_tasks = path->second;
            }
            if (flight.ready_at()) {
                const auto delay = std::max(SimTime::zero(), *flight.ready_at() - flight.target_off_block());
                found->departure_delay = delay;
                total_delay += delay.count();
                metrics.maximum_departure_delay_seconds = std::max(metrics.maximum_departure_delay_seconds, delay.count());
                if (delay == SimTime::zero()) ++metrics.on_time_departures;
                else ++metrics.delayed_turnarounds;
            }
            for (const auto& task_value : flight.tasks()) {
                AircraftMetrics::TaskTiming timing;
                timing.task = task_value.id;
                timing.service = task_value.type;
                timing.state = task_value.status;
                timing.requested_at = task_value.requested_at;
                timing.started_at = task_value.started_at;
                timing.completed_at = task_value.completed_at;
                timing.waiting = task_value.started_at && task_value.requested_at
                    ? *task_value.started_at - *task_value.requested_at : SimTime::zero();
                timing.required_resource = task_value.required_resource;
                timing.assigned_resource = task_value.assigned_resource;
                found->task_timings.push_back(std::move(timing));
                metrics.total_service_task_wait_seconds += found->task_timings.back().waiting.count();
                metrics.maximum_service_task_wait_seconds = std::max(metrics.maximum_service_task_wait_seconds,
                    found->task_timings.back().waiting.count());
                if (task_value.started_at && task_value.completed_at) {
                    service_seconds[task_value.type] += (*task_value.completed_at - *task_value.started_at).count();
                    ++service_counts[task_value.type];
                }
            }
        }
        if (metrics.total_turnarounds != 0) {
            metrics.mean_turnaround_duration_seconds = static_cast<double>(total_turnaround_duration) /
                static_cast<double>(metrics.total_turnarounds);
            metrics.average_turnaround_seconds = metrics.mean_turnaround_duration_seconds;
            metrics.mean_departure_delay_seconds = static_cast<double>(total_delay) /
                static_cast<double>(metrics.total_turnarounds);
            metrics.on_time_departure_rate = static_cast<double>(metrics.on_time_departures) /
                static_cast<double>(metrics.total_turnarounds);
        }
        for (const auto& [type, count] : service_counts) {
            std::size_t capacity = 0;
            if (scenario_.abstract_resource_capacity.contains(type)) capacity = scenario_.abstract_resource_capacity.at(type);
            else capacity = static_cast<std::size_t>(std::ranges::count_if(scenario_.vehicles,
                [type](const ServiceVehicle& service_vehicle) { return service_vehicle.capability() == type; }));
            const auto denominator = static_cast<double>(now_.count()) * static_cast<double>(capacity);
            const auto busy = static_cast<double>(service_seconds[type]);
            metrics.resource_utilization.emplace_back(type, denominator > 0.0 ? busy / denominator : 0.0);
        }
        std::ranges::sort(metrics.resource_utilization, {}, [](const auto& item) { return item.first; });
        metrics.task_reassignments = task_reassignments_;
        metrics.disruption_triggered_replans = disruption_replans_;
        metrics.unresolved_service_requests = fuel_pool_.outstanding_count() + baggage_pool_.outstanding_count();
        metrics.failed_or_timed_out_turnarounds = static_cast<std::size_t>(std::ranges::count_if(
            scenario_.aircraft, [](const Aircraft& flight) { return flight.turnaround_state() == TurnaroundState::Failed; }));
        if (autonomy_fleet_) {
            const auto fleet = autonomy_fleet_->result();
            double fuel_busy_seconds = 0.0;
            double baggage_busy_seconds = 0.0;
            std::size_t fuel_vehicle_count = 0;
            std::size_t baggage_vehicle_count = 0;
            for (const auto& fleet_vehicle : fleet.vehicles) {
                const auto numeric_id = static_cast<std::uint32_t>(std::stoul(fleet_vehicle.id.value));
                const auto vehicle = std::ranges::find(scenario_.vehicles, VehicleId{numeric_id}, &ServiceVehicle::id);
                if (vehicle == scenario_.vehicles.end()) continue;
                if (vehicle->capability() == ServiceType::Fueling) {
                    fuel_busy_seconds += fleet_vehicle.busy_time_s;
                    ++fuel_vehicle_count;
                } else if (vehicle->capability() == ServiceType::Baggage) {
                    baggage_busy_seconds += fleet_vehicle.busy_time_s;
                    ++baggage_vehicle_count;
                }
            }
            const double elapsed_seconds = static_cast<double>(now_.count());
            metrics.fuel_utilization = elapsed_seconds > 0.0 && fuel_vehicle_count > 0
                ? fuel_busy_seconds / (elapsed_seconds * static_cast<double>(fuel_vehicle_count)) : 0.0;
            metrics.baggage_utilization = elapsed_seconds > 0.0 && baggage_vehicle_count > 0
                ? baggage_busy_seconds / (elapsed_seconds * static_cast<double>(baggage_vehicle_count)) : 0.0;
            metrics.fleet_collisions = fleet.collisions;
            metrics.fleet_minimum_separation_m = fleet.minimum_separation_m;
            metrics.fleet_reservation_requests = fleet.reservation_requests;
            metrics.fleet_reservation_contentions = fleet.reservation_contentions;
            metrics.fleet_outstanding_reservations = fleet.outstanding_reservations;
            metrics.fleet_unfinished_requests = fleet.dispatch.unfinished_requests;
            metrics.fleet_reassignments = fleet.dispatch.reassignments;
            metrics.fleet_requests_created = fleet.dispatch.requests_created;
            metrics.fleet_requests_completed = fleet.dispatch.requests_completed;
            metrics.fleet_requests_failed = fleet.dispatch.requests_failed;
            metrics.unresolved_service_requests += fleet.dispatch.unfinished_requests;
        }
    }
    return {seed_, now_, scenario_.aircraft, scenario_.vehicles, log_, event_history_,
        std::move(metrics)};
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
    case EventType::AbstractServiceCompleted:
        handle_abstract_service_completed(AircraftId{event.entity.value}, TaskId{static_cast<std::uint32_t>(event.data)}); break;
    case EventType::TurnaroundTaskEligible:
        handle_task_eligibility(AircraftId{event.entity.value}, TaskId{static_cast<std::uint32_t>(event.data)}); break;
    case EventType::TaskDurationChanged:
        handle_task_duration_change(TaskId{event.entity.value}, SimTime{event.data}); break;
    case EventType::FleetTick:
        fleet_tick_scheduled_ = false;
        handle_fleet_tick(); break;
    case EventType::VehicleOutage:
        handle_vehicle_outage(VehicleId{event.entity.value}); break;
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
    if (scenario_.turnaround_orchestration) {
        auto created = aircraft_event(SimulationEventType::TurnaroundCreated, flight);
        created.turnaround_id = flight.turnaround_id();
        emit(std::move(created));
        schedule_turnaround_tasks(flight);
    } else {
        request_service(flight, ServiceType::Fueling);
        request_service(flight, ServiceType::Baggage);
    }
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
    auto& assigned_task = flight.mutable_task(flight.task(service_vehicle.capability()).id);
    assigned_task.assigned_vehicle = vehicle_id;
    assigned_task.assigned_resource = service_vehicle.name();
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
    if (scenario_.turnaround_orchestration) {
        auto event = aircraft_event(SimulationEventType::TurnaroundTaskDispatched, flight);
        event.task = assigned_task.id;
        event.service = assigned_task.type;
        event.vehicle = vehicle_id;
        event.vehicle_name = service_vehicle.name();
        event.turnaround_id = flight.turnaround_id();
        emit(std::move(event));
        update_turnaround_estimate(flight);
    }
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
    const auto service_duration = scenario_.turnaround_orchestration
        ? flight.task(service_vehicle.capability()).duration : duration(service_vehicle.capability());
    [[maybe_unused]] const auto sequence = events_.schedule(
        now_ + service_duration, EventType::ServiceCompleted,
        {EntityKind::Vehicle, id.value()});
    emit(vehicle_event(SimulationEventType::ServiceStarted, service_vehicle, &flight));
    if (scenario_.turnaround_orchestration) {
        const auto& task = flight.task(service_vehicle.capability());
        auto event = aircraft_event(SimulationEventType::TurnaroundTaskStarted, flight);
        event.task = task.id;
        event.service = task.type;
        event.vehicle = id;
        event.vehicle_name = service_vehicle.name();
        event.turnaround_id = flight.turnaround_id();
        emit(std::move(event));
        update_turnaround_estimate(flight);
    }
}

void Simulation::handle_service_completed(VehicleId id) {
    auto& service_vehicle = vehicle(id);
    const auto aircraft_id = *service_vehicle.assigned_aircraft();
    auto& flight = aircraft(aircraft_id);
    const auto type = service_vehicle.capability();
    const auto previous_aircraft = flight.state();
    flight.complete_task(type, now_);
    emit(vehicle_event(SimulationEventType::ServiceCompleted, service_vehicle, &flight));
    if (scenario_.turnaround_orchestration) {
        const auto& task = flight.task(type);
        auto event = aircraft_event(SimulationEventType::TurnaroundTaskCompleted, flight);
        event.task = task.id;
        event.service = task.type;
        event.vehicle = id;
        event.vehicle_name = service_vehicle.name();
        event.turnaround_id = flight.turnaround_id();
        emit(std::move(event));
    }
    if (flight.services_complete()) {
        emit_aircraft_state(flight, previous_aircraft);
        emit(aircraft_event(SimulationEventType::AircraftReadyForPushback, flight));
        if (scenario_.turnaround_orchestration) {
            auto ready = aircraft_event(SimulationEventType::TurnaroundReadyForDeparture, flight);
            ready.turnaround_id = flight.turnaround_id();
            emit(std::move(ready));
        }
        [[maybe_unused]] const auto sequence = events_.schedule(
            std::max(now_, flight.scheduled_departure()), EventType::AircraftDeparture,
            {EntityKind::Aircraft, aircraft_id.value()});
    }

    auto return_route = find_route(
        scenario_.graph, flight.gate_node(), service_vehicle.depot_node());
    if (!return_route) throw std::runtime_error("no available route back to vehicle depot");
    const auto route = *return_route;
    const auto previous_vehicle = service_vehicle.state();
    const auto service_duration = scenario_.turnaround_orchestration ? flight.task(type).duration : duration(type);
    service_vehicle.finish_service(std::move(*return_route), service_duration, now_);
    emit_vehicle_state(service_vehicle, previous_vehicle);
    [[maybe_unused]] const auto sequence = events_.schedule(
        now_ + route.travel_time, EventType::VehicleArrivalAtDepot,
        {EntityKind::Vehicle, id.value()});
    emit(vehicle_event(SimulationEventType::VehicleDeparted, service_vehicle, &flight, route));
    if (scenario_.turnaround_orchestration) {
        schedule_turnaround_tasks(flight);
        update_turnaround_estimate(flight);
    }
}

void Simulation::complete_turnaround_task(Aircraft& flight, TaskId task_id) {
    const auto type = flight.task(task_id).type;
    flight.complete_task(task_id, now_);
    auto completed = aircraft_event(SimulationEventType::TurnaroundTaskCompleted, flight);
    completed.turnaround_id = flight.turnaround_id();
    completed.task = task_id;
    completed.service = type;
    completed.vehicle = flight.task(task_id).assigned_vehicle;
    completed.vehicle_name = flight.task(task_id).assigned_resource;
    emit(std::move(completed));
    if (flight.services_complete()) {
        auto ready = aircraft_event(SimulationEventType::TurnaroundReadyForDeparture, flight);
        ready.turnaround_id = flight.turnaround_id();
        emit(std::move(ready));
        emit(aircraft_event(SimulationEventType::AircraftReadyForPushback, flight));
        [[maybe_unused]] const auto sequence = events_.schedule(
            std::max(now_, flight.scheduled_departure()), EventType::AircraftDeparture,
            {EntityKind::Aircraft, flight.id().value()});
    }
    for (auto& other : scenario_.aircraft) schedule_turnaround_tasks(other);
}

void Simulation::handle_vehicle_return(VehicleId id) {
    auto& service_vehicle = vehicle(id);
    const auto arrival_route = service_vehicle.active_route();
    const auto previous = service_vehicle.state();
    service_vehicle.arrive_at_depot();
    emit(vehicle_event(SimulationEventType::VehicleArrived, service_vehicle, nullptr, arrival_route));
    emit_vehicle_state(service_vehicle, previous);
    const auto next = pool(service_vehicle.capability()).release(id, now_);
    if (next) dispatch(next->vehicle, next->aircraft);
}

void Simulation::handle_road_event(EdgeId id, bool available) {
    scenario_.graph.set_edge_available(id, available);
    if (autonomy_fleet_) autonomy_fleet_->add_road_event({now_, id, available});
    SimulationEventRecord event{
        available ? SimulationEventType::RoadOpened : SimulationEventType::RoadClosed};
    event.edge = id;
    emit(std::move(event));
}

void Simulation::handle_vehicle_outage(VehicleId id) {
    if (!autonomy_fleet_) throw std::logic_error("vehicle outages require turnaround fleet orchestration");
    const auto fleet_id = autonomy::VehicleId{std::to_string(id.value())};
    const auto requests = autonomy_fleet_->service_requests();
    for (const auto& request : requests) {
        if (!request.assigned_vehicle || *request.assigned_vehicle != fleet_id ||
            request.state == autonomy::ServiceTaskState::Completed ||
            request.state == autonomy::ServiceTaskState::Failed) continue;
        const auto mapped = fleet_task_requests_.find(request.request.id.value);
        if (mapped == fleet_task_requests_.end()) continue;
        auto& flight = aircraft(mapped->second.first);
        auto event = aircraft_event(SimulationEventType::TurnaroundVehicleUnavailable, flight);
        event.turnaround_id = flight.turnaround_id();
        event.task = mapped->second.second;
        event.vehicle = id;
        event.vehicle_name = vehicle(id).name();
        emit(std::move(event));
    }
    autonomy_fleet_->mark_vehicle_unavailable(fleet_id);
    schedule_fleet_tick();
}

void Simulation::schedule_turnaround_tasks(Aircraft& flight) {
    if (!scenario_.turnaround_orchestration || flight.state() == AircraftState::Scheduled ||
        flight.state() == AircraftState::Departed) return;
    update_turnaround_estimate(flight);
    for (auto& task_value : flight.mutable_tasks()) {
        if (task_value.status == TaskStatus::Blocked) {
            const bool dependencies_complete = std::ranges::all_of(task_value.prerequisites, [&](TaskId prerequisite) {
                return flight.task(prerequisite).status == TaskStatus::Completed;
            });
            if (!dependencies_complete) continue;
            if (now_ < task_value.earliest_start) {
                if (!task_value.eligibility_scheduled) {
                    [[maybe_unused]] const auto sequence = events_.schedule(task_value.earliest_start,
                        EventType::TurnaroundTaskEligible, {EntityKind::Aircraft, flight.id().value()}, task_value.id.value());
                    task_value.eligibility_scheduled = true;
                }
                continue;
            }
            flight.make_task_ready(task_value.id);
        }
        if (task_value.status != TaskStatus::Pending && task_value.status != TaskStatus::Waiting) continue;
        if (now_ < task_value.earliest_start) {
            task_value.status = TaskStatus::Blocked;
            if (!task_value.eligibility_scheduled) {
                [[maybe_unused]] const auto sequence = events_.schedule(task_value.earliest_start,
                    EventType::TurnaroundTaskEligible, {EntityKind::Aircraft, flight.id().value()}, task_value.id.value());
                task_value.eligibility_scheduled = true;
            }
            continue;
        }
        if (!task_value.requested_at) {
            auto ready = aircraft_event(SimulationEventType::TurnaroundTaskReady, flight);
            ready.turnaround_id = flight.turnaround_id();
            ready.task = task_value.id;
            ready.service = task_value.type;
            emit(std::move(ready));
        }

        if (uses_mobile_fleet(task_value.type)) {
            std::int64_t priority = 0;
            if (const auto path = critical_paths_.find(flight.id()); path != critical_paths_.end() &&
                std::ranges::find(path->second, task_value.id) != path->second.end()) priority += 10;
            if (const auto slack = schedule_slacks_.find(flight.id()); slack != schedule_slacks_.end()) {
                if (slack->second <= SimTime::zero()) priority += 20;
                else if (slack->second <= SimTime{300}) priority += 10;
                else if (slack->second <= SimTime{600}) priority += 5;
            }
            if (task_value.latest_desirable_completion) {
                const auto remaining_slack = *task_value.latest_desirable_completion - now_ - task_value.duration;
                if (remaining_slack <= SimTime::zero()) priority += 20;
                else if (remaining_slack <= SimTime{300}) priority += 10;
                else if (remaining_slack <= SimTime{600}) priority += 5;
            }
            queue_mobile_task(flight, task_value, priority);
            continue;
        }

        const auto capacity_it = scenario_.abstract_resource_capacity.find(task_value.type);
        const auto capacity = capacity_it == scenario_.abstract_resource_capacity.end() ? 1U : capacity_it->second;
        std::size_t in_use = 0;
        std::vector<std::string> occupied;
        for (const auto& other : scenario_.aircraft) {
            for (const auto& other_task : other.tasks()) {
                if (other_task.type == task_value.type && other_task.status == TaskStatus::InProgress) {
                    ++in_use;
                    occupied.push_back(other_task.assigned_resource);
                }
            }
        }
        if (in_use >= capacity) {
            if (task_value.status == TaskStatus::Pending) flight.mark_task_waiting(task_value.id, now_);
            continue;
        }
        std::size_t slot = 1;
        for (;; ++slot) {
            const auto candidate = std::format("crew:{}:{}", to_string(task_value.type), slot);
            if (std::ranges::find(occupied, candidate) == occupied.end()) {
                task_value.assigned_resource = candidate;
                break;
            }
        }
        const auto resource_name = task_value.assigned_resource;
        flight.assign_task(task_value.id, resource_name);
        flight.start_task(task_value.id, now_);
        auto started = aircraft_event(SimulationEventType::TurnaroundTaskStarted, flight);
        started.turnaround_id = flight.turnaround_id();
        started.task = task_value.id;
        started.service = task_value.type;
        started.vehicle_name = resource_name;
        emit(std::move(started));
        [[maybe_unused]] const auto sequence = events_.schedule(now_ + task_value.duration,
            EventType::AbstractServiceCompleted, {EntityKind::Aircraft, flight.id().value()}, task_value.id.value());
    }
    update_turnaround_estimate(flight);
}

void Simulation::handle_abstract_service_completed(AircraftId id, TaskId task_id) {
    auto& flight = aircraft(id);
    if (flight.task(task_id).status != TaskStatus::InProgress) return;
    complete_turnaround_task(flight, task_id);
}

void Simulation::queue_mobile_task(Aircraft& flight, ServiceTask& task_value, std::int64_t priority) {
    if (!autonomy_fleet_) throw std::logic_error("mobile turnaround task requires a configured fleet");
    const auto request_id = std::format("turnaround-{}-task-{}", flight.id().value(), task_value.id.value());
    if (fleet_task_requests_.contains(request_id)) return;
    auto node = scenario_.graph.node(flight.gate_node()).name;
    if (task_value.type == ServiceType::Baggage || task_value.type == ServiceType::BaggageLoad) {
        const auto staging_name = node + " Baggage Stand";
        if (std::ranges::any_of(scenario_.graph.nodes(), [&](const auto& candidate) {
                return candidate.name == staging_name;
            })) node = staging_name;
    }
    autonomy::ServiceRequest request;
    request.id = autonomy::ServiceRequestId{request_id};
    request.kind = task_value.type == ServiceType::Fueling
        ? autonomy::ServiceKind::FuelService : autonomy::ServiceKind::BaggageDelivery;
    request.required_capability = task_value.type == ServiceType::Fueling ? "fuel_truck" : "baggage_vehicle";
    request.origin = node;
    request.destination = node;
    request.release_time_s = static_cast<double>(now_.count());
    request.priority = static_cast<int>(std::clamp<std::int64_t>(priority, -100000, 100000));
    request.deadline_s = std::max(request.release_time_s,
        static_cast<double>(flight.target_off_block().count()));
    request.service_duration_s = static_cast<double>(task_value.duration.count());
    autonomy_fleet_->add_service_request(std::move(request));
    fleet_task_requests_.emplace(request_id, std::pair{flight.id(), task_value.id});
    if (task_value.status == TaskStatus::Pending) flight.mark_task_waiting(task_value.id, now_);
    auto waiting = aircraft_event(SimulationEventType::ResourceWaitStarted, flight);
    waiting.turnaround_id = flight.turnaround_id();
    waiting.task = task_value.id;
    waiting.service = task_value.type;
    emit(std::move(waiting));
    schedule_fleet_tick();
}

void Simulation::schedule_fleet_tick() {
    if (!autonomy_fleet_ || fleet_tick_scheduled_) return;
    fleet_tick_scheduled_ = true;
    [[maybe_unused]] const auto sequence = events_.schedule(now_ + SimTime{1}, EventType::FleetTick);
}

void Simulation::handle_fleet_tick() {
    if (!autonomy_fleet_) return;
    for (int frame = 0; frame < 50 && !autonomy_fleet_->finished(); ++frame) {
        if (!autonomy_fleet_->advance()) break;
    }
    synchronize_fleet_state();
    const auto requests = autonomy_fleet_->service_requests();
    if (std::ranges::any_of(requests, [](const auto& request) {
            return request.state != autonomy::ServiceTaskState::Completed &&
                   request.state != autonomy::ServiceTaskState::Failed;
        })) schedule_fleet_tick();
}

void Simulation::synchronize_fleet_state() {
    if (!autonomy_fleet_) return;
    for (const auto& request : autonomy_fleet_->service_requests()) {
        const auto mapped = fleet_task_requests_.find(request.request.id.value);
        if (mapped == fleet_task_requests_.end()) continue;
        auto& flight = aircraft(mapped->second.first);
        auto& task_value = flight.mutable_task(mapped->second.second);
        const auto make_vehicle_id = [](const autonomy::VehicleId& id) {
            return VehicleId{static_cast<std::uint32_t>(std::stoul(id.value))};
        };
        const auto reported_reassignments = request.reassignments;
        const auto previous_reassignments = task_value.reassignments;
        switch (request.state) {
        case autonomy::ServiceTaskState::Queued:
            if (task_value.status == TaskStatus::Assigned) flight.requeue_task(task_value.id, now_);
            else if (task_value.status == TaskStatus::Pending) flight.mark_task_waiting(task_value.id, now_);
            break;
        case autonomy::ServiceTaskState::Assigned:
        case autonomy::ServiceTaskState::EnRoute:
            if (!request.assigned_vehicle) break;
            if (task_value.status == TaskStatus::Assigned && task_value.assigned_vehicle != make_vehicle_id(*request.assigned_vehicle))
                flight.requeue_task(task_value.id, now_);
            if (task_value.status == TaskStatus::Pending || task_value.status == TaskStatus::Waiting) {
                const auto id = make_vehicle_id(*request.assigned_vehicle);
                flight.assign_task(task_value.id, vehicle(id).name(), id);
                auto event = aircraft_event(SimulationEventType::TurnaroundTaskDispatched, flight);
                event.turnaround_id = flight.turnaround_id();
                event.task = task_value.id;
                event.service = task_value.type;
                event.vehicle = id;
                event.vehicle_name = vehicle(id).name();
                emit(std::move(event));
            }
            break;
        case autonomy::ServiceTaskState::Servicing:
            if (task_value.status == TaskStatus::Assigned) {
                flight.start_task(task_value.id, now_);
                auto event = aircraft_event(SimulationEventType::TurnaroundTaskStarted, flight);
                event.turnaround_id = flight.turnaround_id();
                event.task = task_value.id;
                event.service = task_value.type;
                event.vehicle = task_value.assigned_vehicle;
                event.vehicle_name = task_value.assigned_resource;
                emit(std::move(event));
            }
            break;
        case autonomy::ServiceTaskState::Completed:
            if (task_value.status != TaskStatus::Completed) {
                if (task_value.status != TaskStatus::InProgress) {
                    if (!request.assigned_vehicle)
                        throw std::logic_error("completed fleet request has no assigned vehicle");
                    const auto id = make_vehicle_id(*request.assigned_vehicle);
                    if (task_value.status == TaskStatus::Pending || task_value.status == TaskStatus::Waiting)
                        flight.assign_task(task_value.id, vehicle(id).name(), id);
                    const auto inferred_start = SimTime{static_cast<std::int64_t>(std::floor(
                        request.completed_at_s - request.request.service_duration_s))};
                    flight.start_task(task_value.id, inferred_start);
                    auto started = aircraft_event(SimulationEventType::TurnaroundTaskStarted, flight);
                    started.turnaround_id = flight.turnaround_id();
                    started.task = task_value.id;
                    started.service = task_value.type;
                    started.vehicle = id;
                    started.vehicle_name = vehicle(id).name();
                    emit(std::move(started));
                }
                complete_turnaround_task(flight, task_value.id);
            }
            break;
        case autonomy::ServiceTaskState::Failed:
            if (task_value.status != TaskStatus::Failed) {
                flight.fail_task(task_value.id, "fleet_dispatch_failed");
                auto event = aircraft_event(SimulationEventType::TurnaroundTaskFailed, flight);
                event.turnaround_id = flight.turnaround_id();
                event.task = task_value.id;
                event.service = task_value.type;
                event.detail = "fleet_dispatch_failed";
                emit(std::move(event));
                flight.fail_turnaround("fleet_dispatch_failed");
                auto failed = aircraft_event(SimulationEventType::TurnaroundFailed, flight);
                failed.turnaround_id = flight.turnaround_id();
                failed.detail = flight.failure_reason();
                emit(std::move(failed));
            }
            break;
        }
        if (reported_reassignments > previous_reassignments) {
            task_value.reassignments = reported_reassignments;
            ++task_reassignments_;
            auto event = aircraft_event(SimulationEventType::TurnaroundTaskReassigned, flight);
            event.turnaround_id = flight.turnaround_id();
            event.task = task_value.id;
            event.service = task_value.type;
            if (request.assigned_vehicle) {
                event.vehicle = make_vehicle_id(*request.assigned_vehicle);
                event.vehicle_name = vehicle(*event.vehicle).name();
            }
            emit(std::move(event));
        }
    }
}

void Simulation::handle_task_eligibility(AircraftId id, TaskId task_id) {
    auto& flight = aircraft(id);
    auto& task_value = flight.mutable_task(task_id);
    task_value.eligibility_scheduled = false;
    if (task_value.status != TaskStatus::Blocked) return;
    if (!std::ranges::all_of(task_value.prerequisites, [&](TaskId prerequisite) {
            return flight.task(prerequisite).status == TaskStatus::Completed;
        })) return;
    if (now_ < task_value.earliest_start) return;
    flight.make_task_ready(task_id);
    schedule_turnaround_tasks(flight);
}

void Simulation::handle_task_duration_change(TaskId task_id, SimTime duration_value) {
    if (duration_value <= SimTime::zero()) throw std::logic_error("disruption duration must be positive");
    const auto owner = std::ranges::find_if(scenario_.aircraft, [&](const Aircraft& flight) {
        return std::ranges::any_of(flight.tasks(), [task_id](const ServiceTask& task_value) { return task_value.id == task_id; });
    });
    if (owner == scenario_.aircraft.end()) throw std::logic_error("disruption references unknown task");
    auto& task_value = owner->mutable_task(task_id);
    if (task_value.status == TaskStatus::InProgress || task_value.status == TaskStatus::Completed) {
        throw std::logic_error("turnaround disruption targets a task after service start");
    }
    owner->set_task_duration(task_id, duration_value);
    if (autonomy_fleet_) {
        const auto request = std::ranges::find_if(fleet_task_requests_, [&](const auto& item) {
            return item.second.second == task_id;
        });
        if (request != fleet_task_requests_.end()) {
            autonomy_fleet_->update_service_duration(autonomy::ServiceRequestId{request->first},
                static_cast<double>(duration_value.count()));
        }
    }
    ++disruption_replans_;
    auto event = aircraft_event(SimulationEventType::TurnaroundDisruptionDetected, *owner);
    event.task = task_id;
    event.service = task_value.type;
    event.turnaround_id = owner->turnaround_id();
    event.task_duration = duration_value;
    emit(std::move(event));
    update_turnaround_estimate(*owner);
    for (auto& flight : scenario_.aircraft) schedule_turnaround_tasks(flight);
}

void Simulation::update_turnaround_estimate(const Aircraft& flight) {
    if (!scenario_.turnaround_orchestration) return;
    const auto& tasks = flight.tasks();
    std::vector<SimTime> finish(tasks.size(), now_);
    std::vector<std::optional<std::size_t>> parent(tasks.size());
    std::vector<bool> visited(tasks.size(), false);
    std::function<SimTime(std::size_t)> estimate = [&](std::size_t index) -> SimTime {
        if (visited[index]) return finish[index];
        visited[index] = true;
        const auto& task_value = tasks[index];
        if (task_value.status == TaskStatus::Completed && task_value.completed_at) {
            finish[index] = *task_value.completed_at;
            return finish[index];
        }
        SimTime start = std::max(now_, task_value.earliest_start);
        for (const auto prerequisite : task_value.prerequisites) {
            const auto found = std::ranges::find(tasks, prerequisite, &ServiceTask::id);
            if (found == tasks.end()) continue;
            const auto predecessor = static_cast<std::size_t>(found - tasks.begin());
            const auto predecessor_finish = estimate(predecessor);
            if (predecessor_finish >= start) { start = predecessor_finish; parent[index] = predecessor; }
        }
        SimTime remaining = task_value.duration;
        if (task_value.status == TaskStatus::InProgress && task_value.started_at) {
            remaining = std::max(SimTime::zero(), task_value.duration - (now_ - *task_value.started_at));
            if (task_value.assigned_vehicle) {
                const auto& assigned = vehicle(*task_value.assigned_vehicle);
                if (assigned.journey_arrival_time() && *assigned.journey_arrival_time() > now_) {
                    start = std::max(start, *assigned.journey_arrival_time());
                }
            }
        } else if (task_value.status == TaskStatus::Assigned && task_value.assigned_vehicle) {
            const auto& assigned = vehicle(*task_value.assigned_vehicle);
            if (assigned.journey_arrival_time()) start = std::max(start, *assigned.journey_arrival_time());
        }
        finish[index] = start + remaining;
        return finish[index];
    };
    SimTime ready = flight.actual_arrival().value_or(flight.scheduled_arrival());
    std::optional<std::size_t> terminal;
    for (std::size_t index = 0; index < tasks.size(); ++index) {
        const auto value = estimate(index);
        if (!terminal || value > ready) { ready = value; terminal = index; }
    }
    std::vector<TaskId> path;
    while (terminal) {
        path.push_back(tasks[*terminal].id);
        terminal = parent[*terminal];
    }
    std::ranges::reverse(path);
    const auto old_path = critical_paths_.find(flight.id());
    if (flight.services_complete() && old_path != critical_paths_.end()) {
        // Preserve the last computed dependency chain as the completion
        // estimate collapses to the already-completed terminal task.
        path = old_path->second;
        ready = flight.ready_at().value_or(ready);
    }
    if (old_path == critical_paths_.end() || old_path->second != path) {
        critical_paths_[flight.id()] = path;
        auto changed = aircraft_event(SimulationEventType::TurnaroundCriticalPathChanged, flight);
        changed.turnaround_id = flight.turnaround_id();
        changed.estimated_ready_time = ready;
        changed.schedule_slack = flight.target_off_block() - ready;
        emit(std::move(changed));
    }
    estimated_ready_times_[flight.id()] = ready;
    schedule_slacks_[flight.id()] = flight.target_off_block() - ready;
    const bool late = ready > flight.target_off_block();
    if (late && !predicted_late_[flight.id()]) {
        auto event = aircraft_event(SimulationEventType::TurnaroundPredictedLate, flight);
        event.turnaround_id = flight.turnaround_id();
        event.estimated_ready_time = ready;
        event.schedule_slack = flight.target_off_block() - ready;
        emit(std::move(event));
    }
    predicted_late_[flight.id()] = late;
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
    if (history_policy_ == SimulationHistoryPolicy::Retain) {
        event_history_.push_back(std::move(event));
        log_.push_back(std::format("{}  {}", format_sim_time(now_), format_event(event_history_.back())));
        for (auto* sink : event_sinks_) sink->on_event(event_history_.back());
    } else {
        for (auto* sink : event_sinks_) sink->on_event(event);
    }
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
