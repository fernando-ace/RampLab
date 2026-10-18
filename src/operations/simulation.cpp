#include "airside/operations/simulation.hpp"

#include "airside/autonomy/fleet.hpp"
#include "airside/routing/astar.hpp"
#include "airside/operations/surface_safety.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <functional>
#include <limits>
#include <set>
#include <stdexcept>
#include <tuple>
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

std::optional<ResourcePool> make_resource_pool(const Scenario& scenario, ServiceType type) {
    auto ids = vehicle_ids(scenario, type);
    if (ids.empty()) return std::nullopt;
    return ResourcePool(std::move(ids));
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
    event.turnaround_id = aircraft.turnaround_id();
    return event;
}

bool uses_mobile_fleet(ServiceType type) {
    switch (type) {
    case ServiceType::Fueling: case ServiceType::Baggage: case ServiceType::Catering:
    case ServiceType::BaggageLoad: case ServiceType::LavatoryService: case ServiceType::PotableWater:
    case ServiceType::GroundPower: case ServiceType::Pushback: case ServiceType::PassengerStairs:
    case ServiceType::AirStart: return true;
    default: return false;
    }
}

double point_to_segment_distance_m(const Vec2& point, const Vec2& start, const Vec2& end) {
    const double dx = end.x_m - start.x_m;
    const double dy = end.y_m - start.y_m;
    const double length_squared = dx * dx + dy * dy;
    const double fraction = length_squared == 0.0 ? 0.0 : std::clamp(
        ((point.x_m - start.x_m) * dx + (point.y_m - start.y_m) * dy) / length_squared,
        0.0, 1.0);
    return std::hypot(point.x_m - (start.x_m + fraction * dx),
        point.y_m - (start.y_m + fraction * dy));
}

double point_to_edge_distance_m(const AirportGraph& graph, const RoadEdge& edge, const Vec2& point) {
    const auto& line = edge.centerline;
    if (line.size() < 2U)
        return point_to_segment_distance_m(point, graph.node(edge.from).position, graph.node(edge.to).position);
    double minimum = std::numeric_limits<double>::infinity();
    for (std::size_t index = 1; index < line.size(); ++index)
        minimum = std::min(minimum, point_to_segment_distance_m(point, line[index - 1], line[index]));
    return minimum;
}

std::string fleet_capability(ServiceType type) {
    switch (type) {
    case ServiceType::Fueling: return "fuel_truck";
    case ServiceType::Baggage: case ServiceType::BaggageLoad: return "baggage_vehicle";
    case ServiceType::Catering: return "catering_truck";
    case ServiceType::LavatoryService: return "lavatory_truck";
    case ServiceType::PotableWater: return "water_truck";
    case ServiceType::GroundPower: return "gpu";
    case ServiceType::Pushback: return "pushback_tug";
    case ServiceType::PassengerStairs: return "passenger_stairs";
    case ServiceType::AirStart: return "air_start_unit";
    default: return "";
    }
}

}  // namespace

Simulation::Simulation(Scenario scenario, std::uint64_t seed, SimulationHistoryPolicy history_policy)
    : scenario_(scenario), replay_scenario_(std::move(scenario)), seed_(seed), random_(seed),
      fuel_pool_(make_resource_pool(scenario_, ServiceType::Fueling)),
      baggage_pool_(make_resource_pool(scenario_, ServiceType::Baggage)), history_policy_(history_policy) {
    if (!scenario_.turnaround_orchestration) {
        for (const auto type : {ServiceType::Fueling, ServiceType::Baggage}) {
            if (!scenario_.service_durations.contains(type) || duration(type) <= SimTime::zero())
                throw std::invalid_argument("legacy service scenarios require positive fuel and baggage durations");
        }
    }
    for (const auto& [type, service_duration] : scenario_.service_durations) {
        static_cast<void>(type);
        if (service_duration <= SimTime::zero()) throw std::invalid_argument("service durations must be positive");
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
            const auto capability = fleet_capability(service_vehicle.capability());
            if (capability.empty()) throw std::invalid_argument("turnaround fleet vehicle has no mobile capability");
            const auto return_capability = "ramplab_return_" + id.value;
            std::optional<std::string> outage_safe_node;
            if (const auto refuge = service_vehicle.outage_safe_node())
                outage_safe_node = scenario_.graph.node(*refuge).name;
            std::vector<std::string> capabilities{capability};
            if (scenario_.surface_operations && scenario_.surface_operations->return_service_vehicles_to_depot_after_task)
                capabilities.push_back(return_capability);
            fleet_scenario.dispatch_fleet.push_back({id, std::move(capabilities), depot.name,
                autonomy::DispatchVehicleState::Idle, std::nullopt, {}, std::move(outage_safe_node)});
        }
        autonomy_fleet_ = std::make_unique<autonomy::FleetSimulation>(std::move(fleet_scenario), seed_);
    }
    if (scenario_.surface_operations && !scenario_.turnaround_orchestration &&
        std::ranges::any_of(scenario_.aircraft, [](const auto& aircraft) {
            return aircraft.operation_type() != AircraftOperationType::ArrivalOnly;
        }))
        throw std::invalid_argument("surface operations require turnaround orchestration");
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
    for (const auto& [id, state] : surface_aircraft_) {
        if (state.phase == SurfaceAircraftState::Phase::Runway) result.runway_owner = id;
        if (state.phase == SurfaceAircraftState::Phase::ArrivalQueue ||
            state.phase == SurfaceAircraftState::Phase::WaitingForRunway) result.runway_queue.push_back(id);
    }
    std::ranges::sort(result.runway_queue, [this](AircraftId left, AircraftId right) {
        const auto& l = surface_aircraft_.at(left);
        const auto& r = surface_aircraft_.at(right);
        return l.queued_at == r.queued_at ? left < right : l.queued_at < r.queued_at;
    });
    result.aircraft.reserve(scenario_.aircraft.size());
    for (const auto& flight : scenario_.aircraft) {
        AircraftSnapshot item{flight.id(), flight.flight_number(), flight.state(), flight.gate(),
            std::nullopt, flight.scheduled_arrival(), flight.actual_arrival(),
            flight.scheduled_departure(), flight.actual_departure(), {}};
        if (flight.state() != AircraftState::Scheduled && flight.state() != AircraftState::Departed) {
            item.logical_node = flight.gate_node();
        }
        if (const auto surface = surface_aircraft_.find(flight.id()); surface != surface_aircraft_.end()) {
            const auto& state = surface->second;
            item.operation_type = flight.operation_type() == AircraftOperationType::ArrivalOnly ? "arrival" :
                flight.operation_type() == AircraftOperationType::ArrivalTurnaround ? "arrival_turnaround" : "departure";
            switch (state.phase) {
            case SurfaceAircraftState::Phase::None: item.surface_state = flight.state() == AircraftState::Scheduled ? "Scheduled" : "ParkedAtGate"; break;
            case SurfaceAircraftState::Phase::ArrivalQueue: item.surface_state = "ArrivalQueue"; break;
            case SurfaceAircraftState::Phase::WaitingForGate: item.surface_state = "WaitingForGate"; break;
            case SurfaceAircraftState::Phase::WaitingForPushback: item.surface_state = "ReadyForPushback"; break;
            case SurfaceAircraftState::Phase::Pushback: item.surface_state = "Pushback"; break;
            case SurfaceAircraftState::Phase::Taxiing: item.surface_state = state.arrival_operation ? "TaxiingToGate" : "Taxiing"; break;
            case SurfaceAircraftState::Phase::WaitingForTraffic: item.surface_state = "WaitingForTraffic"; break;
            case SurfaceAircraftState::Phase::WaitingForRunway: item.surface_state = "WaitingForRunway"; break;
            case SurfaceAircraftState::Phase::Runway: item.surface_state = "RunwayOccupied"; break;
            case SurfaceAircraftState::Phase::Departed: item.surface_state = "Departed"; break;
            case SurfaceAircraftState::Phase::Arrived: item.surface_state = "Arrived"; break;
            case SurfaceAircraftState::Phase::Failed: item.surface_state = "SafelyStopped"; break;
            }
            if (state.clearance_waiting && state.phase == SurfaceAircraftState::Phase::Taxiing)
                item.surface_state = "WaitingForTraffic";
            item.surface_node = state.node;
            item.surface_route = state.route_nodes;
            item.surface_reroutes = state.reroutes;
            item.taxi_distance_m = state.taxi_distance_m;
            item.surface_wait_duration = state.accumulated_wait +
                (state.wait_started ? now_ - *state.wait_started : SimTime::zero());
            item.pushback_started_at = state.pushback_started_at;
            item.pushback_completed_at = state.pushback_completed_at;
            item.taxi_started_at = state.taxi_started_at;
            item.taxi_completed_at = state.taxi_completed_at;
            item.runway_queue_entered_at = state.runway_queue_entered_at;
            item.actual_surface_departure = state.actual_surface_departure;
            item.runway_clearance_at = state.runway_clearance_at;
            item.runway_release_at = state.runway_release_at;
            item.arrival_runway_request_time = state.arrival_runway_request_time;
            item.arrival_runway_clearance_time = state.arrival_runway_clearance_time;
            item.arrival_runway_release_time = state.arrival_runway_release_time;
            item.arrival_runway_wait_duration = state.arrival_runway_wait;
            item.departure_runway_request_time = state.departure_runway_request_time;
            item.departure_runway_clearance_time = state.departure_runway_clearance_time;
            item.departure_runway_release_time = state.departure_runway_release_time;
            item.departure_runway_wait_duration = state.departure_runway_wait;
            item.arrival_gate_at = state.arrival_gate_at;
            item.gate_wait_duration = state.gate_wait_duration;
            item.gate_occupancy_duration = state.gate_occupancy_duration;
            item.gate_released_at = state.gate_released_at;
            item.arrival_taxi_started_at = state.arrival_taxi_started_at;
            item.arrival_taxi_completed_at = state.arrival_taxi_completed_at;
            item.arrival_taxi_distance_m = state.arrival_taxi_distance_m;
            item.departure_taxi_started_at = state.departure_taxi_started_at;
            item.departure_taxi_completed_at = state.departure_taxi_completed_at;
            item.departure_taxi_distance_m = state.departure_taxi_distance_m;
            item.gate_assigned_at = state.gate_assigned_at;
            item.runway_wait_duration = state.runway_wait;
            if (state.clearance_waiting) item.surface_wait_reason = "aircraft_ground_vehicle_clearance";
            else if (state.phase == SurfaceAircraftState::Phase::WaitingForTraffic) item.surface_wait_reason = "edge_or_intersection_reserved";
            if (state.phase == SurfaceAircraftState::Phase::WaitingForPushback) item.surface_wait_reason = "pushback_area_reserved";
            item.surface_speed_mps = state.phase == SurfaceAircraftState::Phase::Taxiing && !state.clearance_waiting
                ? scenario_.surface_operations->aircraft_speed_mps : 0.0;
            if (state.phase != SurfaceAircraftState::Phase::Departed && state.phase != SurfaceAircraftState::Phase::Arrived &&
                (flight.state() != AircraftState::Scheduled || state.phase == SurfaceAircraftState::Phase::ArrivalQueue)) {
                item.surface_position_m = scenario_.graph.node(state.node).position;
                const bool departure_only_scenario = std::ranges::none_of(scenario_.aircraft, [](const Aircraft& candidate) {
                    return candidate.operation_type() == AircraftOperationType::ArrivalOnly;
                });
                if (state.phase == SurfaceAircraftState::Phase::WaitingForRunway ||
                    state.phase == SurfaceAircraftState::Phase::ArrivalQueue ||
                    (state.phase == SurfaceAircraftState::Phase::Runway && departure_only_scenario)) {
                    item.surface_position_m->x_m -= scenario_.surface_operations->departure_queue_spacing_m *
                        static_cast<double>(state.queue_slot + 1U);
                }
            }
            if (state.phase_end && state.phase_started && state.phase == SurfaceAircraftState::Phase::Taxiing &&
                state.edge_index < state.route_edges.size()) {
                const auto duration = (*state.phase_end - *state.phase_started).count();
                const auto elapsed = std::clamp((now_ - *state.phase_started).count(), std::int64_t{0}, duration);
                const double fraction = duration > 0 ? static_cast<double>(elapsed) / static_cast<double>(duration) : 1.0;
                const auto& movement_edge = scenario_.graph.edge(state.route_edges[state.edge_index]);
                const auto from_node = state.route_nodes[state.edge_index];
                item.surface_position_m = scenario_.graph.edge_position(movement_edge, from_node, fraction);
                const auto ahead = scenario_.graph.edge_position(movement_edge, from_node, std::min(1.0, fraction + 0.01));
                item.surface_heading_rad = std::atan2(ahead.y_m - item.surface_position_m->y_m,
                    ahead.x_m - item.surface_position_m->x_m);
                item.surface_next_waypoint = scenario_.graph.node(state.route_nodes[state.edge_index + 1]).name;
            }
        } else if (scenario_.surface_operations) {
            item.operation_type = flight.operation_type() == AircraftOperationType::ArrivalOnly ? "arrival" :
                flight.operation_type() == AircraftOperationType::ArrivalTurnaround ? "arrival_turnaround" : "departure";
            item.surface_state = flight.state() == AircraftState::Scheduled ? "Scheduled" :
                flight.state() == AircraftState::Departed ? "Departed" : "ParkedAtGate";
            item.surface_node = flight.gate_node();
            if (flight.state() != AircraftState::Scheduled && flight.state() != AircraftState::Departed)
                item.surface_position_m = scenario_.graph.node(flight.gate_node()).position;
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
        if (flight.operation_type() == AircraftOperationType::ArrivalOnly) continue;
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
            [](const Aircraft& flight) { return flight.operation_type() != AircraftOperationType::ArrivalOnly && flight.actual_departure().has_value(); });
    }
    auto metrics = calculate_metrics(scenario_.turnaround_orchestration ? completed_aircraft : scenario_.aircraft,
        scenario_.vehicles, now_);
    metrics.agent_decisions = agent_decisions_.size();
    metrics.agent_route_selections = static_cast<std::size_t>(std::ranges::count_if(agent_decisions_,
        [](const auto& item) { return item.action == agents::ActionType::PlanRoute; }));
    metrics.agent_replans = static_cast<std::size_t>(std::ranges::count_if(agent_decisions_, [](const auto& item) {
        return item.action == agents::ActionType::PlanRoute && item.reason != agents::ReasonCode::ObjectiveCreated;
    }));
    metrics.agent_objectives_completed = static_cast<std::size_t>(std::ranges::count_if(agent_decisions_,
        [](const auto& item) { return item.action == agents::ActionType::CompleteObjective; }));
    metrics.agent_objectives_failed = static_cast<std::size_t>(std::ranges::count_if(agent_decisions_,
        [](const auto& item) { return item.action == agents::ActionType::FailObjective; }));
    metrics.agent_operator_interventions = static_cast<std::size_t>(std::ranges::count_if(agent_decisions_,
        [](const auto& item) { return item.operator_triggered; }));
    if (scenario_.turnaround_orchestration) {
        metrics.total_turnarounds = static_cast<std::size_t>(std::ranges::count_if(scenario_.aircraft,
            [](const Aircraft& flight) { return flight.operation_type() != AircraftOperationType::ArrivalOnly; }));
        std::int64_t total_delay = 0;
        std::int64_t total_turnaround_duration = 0;
        std::unordered_map<ServiceType, std::int64_t> service_seconds;
        std::unordered_map<ServiceType, std::size_t> service_counts;
        for (const auto& flight : scenario_.aircraft) {
            if (flight.operation_type() == AircraftOperationType::ArrivalOnly) continue;
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
            if (flight.actual_departure()) {
                const auto delay = std::max(SimTime::zero(), *flight.actual_departure() - flight.scheduled_departure());
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
        metrics.unresolved_service_requests = (fuel_pool_ ? fuel_pool_->outstanding_count() : 0)
            + (baggage_pool_ ? baggage_pool_->outstanding_count() : 0);
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
            metrics.fleet_deadlock_unresolved = fleet.unresolved_deadlock;
            metrics.fleet_failure_reason = fleet.failure_reason;
            metrics.fleet_deadlock_count = fleet.deadlock_count;
            for (const auto& dependency : fleet.wait_dependencies)
                metrics.fleet_wait_dependencies.push_back(std::format("{} -> {} [{}] {:.2f}s",
                    dependency.waiting_vehicle.value, dependency.blocking_vehicle.value,
                    dependency.resource, dependency.wait_duration_s));
            for (const auto& vehicle_id : fleet.deadlocked_vehicles)
                metrics.fleet_deadlocked_vehicles.push_back(vehicle_id.value);
            for (const auto& event : fleet.events) {
                switch (event.kind) {
                    case autonomy::TrafficEventKind::Waiting:
                    case autonomy::TrafficEventKind::WaitEnded:
                    case autonomy::TrafficEventKind::DeadlockDetected:
                    case autonomy::TrafficEventKind::DeadlockRecovery:
                    case autonomy::TrafficEventKind::RecoveryResolved:
                    case autonomy::TrafficEventKind::RetreatSelected:
                    case autonomy::TrafficEventKind::RetreatStarted:
                    case autonomy::TrafficEventKind::RetreatResourceReleased:
                    case autonomy::TrafficEventKind::RetreatCompleted:
                    case autonomy::TrafficEventKind::MissionResumed:
                    case autonomy::TrafficEventKind::RetreatFailed:
                    case autonomy::TrafficEventKind::DeadlockUnresolved:
                        metrics.fleet_recovery_events.push_back(std::format(
                            "{:.2f}s {} vehicle={} other={} resource={} position={:.2f}:{:.2f} progress={:.2f}",
                            event.time_s, autonomy::to_string(event.kind), event.vehicle.value,
                            event.other.value, event.resource, event.position.x_m, event.position.y_m,
                            event.progress_m));
                        break;
                    default:
                        break;
                }
            }
            for (const auto& [resource, owner] : fleet.held_reservation_owners)
                metrics.fleet_held_reservation_owners.push_back(std::format("{}={}", resource, owner.value));
            for (const auto& vehicle_state : autonomy_fleet_->snapshots()) {
                std::string route;
                for (const auto& point : vehicle_state.autonomy.mission.waypoints) {
                    if (!route.empty()) route += "->";
                    route += std::format("{:.1f}:{:.1f}", point.x_m, point.y_m);
                }
                metrics.fleet_vehicle_diagnostics.push_back(std::format(
                    "{} state={} request={} position={:.2f}:{:.2f} goal={} recovery={} resource={} route=[{}]",
                    vehicle_state.id.value, autonomy::to_string(vehicle_state.dispatch_state),
                    vehicle_state.current_request ? vehicle_state.current_request->value : "-",
                    vehicle_state.autonomy.ground_truth.position.x_m, vehicle_state.autonomy.ground_truth.position.y_m,
                    vehicle_state.goal_node, vehicle_state.recovery_state, vehicle_state.recovery_resource, route));
            }
            metrics.unresolved_service_requests += fleet.dispatch.unfinished_requests;
        }
        if (scenario_.surface_operations) {
            metrics.surface_total_aircraft = scenario_.aircraft.size();
            metrics.surface_wait_events = surface_wait_events_;
            metrics.max_simultaneous_taxiing_aircraft = maximum_simultaneous_taxiing_;
            metrics.minimum_aircraft_separation_m = std::isfinite(minimum_aircraft_separation_m_)
                ? minimum_aircraft_separation_m_ : 0.0;
            metrics.minimum_aircraft_ground_separation_m = std::isfinite(minimum_aircraft_ground_separation_m_)
                ? minimum_aircraft_ground_separation_m_ : 0.0;
            metrics.surface_aircraft_aircraft_collisions = surface_aircraft_aircraft_collisions_;
            metrics.surface_aircraft_ground_collisions = surface_aircraft_ground_collisions_;
            for (const auto& [id, state] : surface_aircraft_) {
                const auto flight_it = std::ranges::find(scenario_.aircraft, id, &Aircraft::id);
                const auto& flight = *flight_it;
                const bool inbound = flight.operation_type() != AircraftOperationType::Turnaround;
                auto metric = std::ranges::find(metrics.aircraft, id, &AircraftMetrics::id);
                if (metric == metrics.aircraft.end()) {
                    metrics.aircraft.push_back(AircraftMetrics{id, flight.flight_number()});
                    metric = std::prev(metrics.aircraft.end());
                }
                metric->operation_type = flight.operation_type() == AircraftOperationType::ArrivalTurnaround
                    ? "arrival_turnaround" : inbound ? "arrival" : "departure";
                metric->runway_request_time = state.runway_queue_entered_at;
                metric->runway_clearance_time = state.runway_clearance_at;
                metric->runway_wait = state.arrival_runway_wait + state.departure_runway_wait;
                metric->runway_release_time = state.runway_release_at;
                metric->runway_occupancy = state.runway_occupied;
                metric->arrival_taxi_distance_m = inbound ? state.arrival_taxi_distance_m : 0.0;
                metric->arrival_taxi_time = inbound && state.arrival_taxi_started_at && state.arrival_taxi_completed_at
                    ? *state.arrival_taxi_completed_at - *state.arrival_taxi_started_at : SimTime::zero();
                metric->departure_taxi_distance_m = state.departure_taxi_distance_m;
                metric->departure_taxi_time = state.departure_taxi_started_at && state.departure_taxi_completed_at
                    ? *state.departure_taxi_completed_at - *state.departure_taxi_started_at : SimTime::zero();
                metric->gate_arrival_time = state.arrival_gate_at;
                metric->gate_wait = state.gate_wait_duration;
                metric->gate_occupancy = state.gate_occupancy_duration;
                metric->pushback_start_time = state.pushback_started_at;
                metric->arrival_to_departure = state.actual_surface_departure && flight.actual_arrival()
                    ? *state.actual_surface_departure - *flight.actual_arrival() : SimTime::zero();
                metric->surface_departure_time = state.actual_surface_departure;
                metric->total_operational_delay = inbound && state.arrival_gate_at
                    ? std::max(SimTime::zero(), *state.arrival_gate_at - flight.scheduled_arrival())
                    : flight.departure_delay().value_or(SimTime::zero());
                metrics.surface_departed_aircraft += state.actual_surface_departure.has_value() ? 1U : 0U;
                metrics.surface_arrived_aircraft += state.arrival_gate_at.has_value() ? 1U : 0U;
                metrics.gate_assignments += state.gate_assigned_at.has_value() ? 1U : 0U;
                metrics.gate_wait_seconds += state.gate_wait_duration.count();
                metrics.gate_occupancy_seconds += state.gate_occupancy_duration.count();
                metrics.arrival_to_departure_seconds += metric->arrival_to_departure.count();
                metrics.runway_operations_completed += (state.arrival_runway_release_time.has_value() ? 1U : 0U) +
                    (state.departure_runway_release_time.has_value() ? 1U : 0U);
                metrics.arrival_runway_wait_seconds += state.arrival_runway_wait.count();
                metrics.departure_runway_wait_seconds += state.departure_runway_wait.count();
                metrics.runway_occupied_seconds += state.arrival_runway_occupied.count() + state.departure_runway_occupied.count();
                metrics.surface_safe_failures += state.phase == SurfaceAircraftState::Phase::Failed ? 1U : 0U;
                metrics.surface_reroutes += state.reroutes;
                metrics.surface_wait_seconds += state.accumulated_wait.count();
                metrics.surface_taxi_distance_m += state.taxi_distance_m;
                if (state.arrival_taxi_started_at && state.arrival_taxi_completed_at) {
                    metrics.arrival_taxi_seconds += (*state.arrival_taxi_completed_at - *state.arrival_taxi_started_at).count();
                    metrics.surface_taxi_seconds += (*state.arrival_taxi_completed_at - *state.arrival_taxi_started_at).count();
                }
                if (state.departure_taxi_started_at && state.departure_taxi_completed_at) {
                    metrics.departure_taxi_seconds += (*state.departure_taxi_completed_at - *state.departure_taxi_started_at).count();
                    metrics.surface_taxi_seconds += (*state.departure_taxi_completed_at - *state.departure_taxi_started_at).count();
                }
                metrics.arrival_taxi_distance_m += state.arrival_taxi_distance_m;
                metrics.departure_taxi_distance_m += state.departure_taxi_distance_m;
                metrics.runway_queue_seconds += state.arrival_runway_wait.count() + state.departure_runway_wait.count();
            }
            metrics.maximum_runway_queue_depth = maximum_runway_queue_depth_;
            const auto runway_wait_total = metrics.arrival_runway_wait_seconds + metrics.departure_runway_wait_seconds;
            metrics.average_runway_wait_seconds = metrics.runway_operations_completed > 0
                ? static_cast<double>(runway_wait_total) / static_cast<double>(metrics.runway_operations_completed) : 0.0;
            metrics.runway_utilization = now_ > SimTime::zero()
                ? static_cast<double>(metrics.runway_occupied_seconds) / static_cast<double>(now_.count()) : 0.0;
            const double elapsed_seconds = static_cast<double>(now_.count());
            metrics.surface_departure_throughput_per_hour = elapsed_seconds > 0.0
                ? static_cast<double>(metrics.surface_departed_aircraft) * 3600.0 / elapsed_seconds : 0.0;
        }
    }
    return {seed_, now_, scenario_.aircraft, scenario_.vehicles, log_, event_history_,
        std::move(metrics), agent_decisions_, recovery_epochs_};
}

SimTime Simulation::current_time() const noexcept { return now_; }
const std::vector<SimulationEventRecord>& Simulation::event_history() const noexcept { return event_history_; }

ReplayBoundaryVerification Simulation::verify_replay_boundary() const {
    ReplayBoundaryVerification verification;
    const auto has_operator_intervention = std::ranges::any_of(event_history_, [](const auto& event) {
        return event.type == SimulationEventType::OperatorIntervention;
    });
    if (has_operator_intervention) {
        verification.reason = "runtime_intervention_provenance_unavailable";
        return verification;
    }

    Scenario replay_scenario = replay_scenario_;
    replay_scenario.recovery_mode = RecoveryMode::Reactive;
    Simulation replay{std::move(replay_scenario), seed_, SimulationHistoryPolicy::Retain};
    while (const auto next = replay.next_event_time()) {
        if (*next > now_) break;
        if (!replay.advance()) break;
    }
    verification.matched_events = std::min(event_history_.size(), replay.event_history().size());
    const bool same_events = event_history_ == replay.event_history();
    const bool same_snapshot = snapshot() == replay.snapshot();
    RecoveryState recovery_state;
    recovery_state.snapshot = snapshot();
    recovery_state.revealed_disruptions = revealed_disruptions_;
    verification.recovery_fingerprint = recovery_fingerprint(recovery_state);
    verification.equivalent = replay.current_time() == now_ && same_events && same_snapshot;
    verification.reason = verification.equivalent ? "equivalent" : "replay_boundary_mismatch";
    return verification;
}

void Simulation::schedule_surface_availability(EdgeId edge, bool available) {
    (void)scenario_.graph.edge(edge);
    (void)events_.schedule(now_, EventType::OperatorSurfaceAvailability,
        {EntityKind::Edge, edge.value()}, available ? 1 : 0);
}

void Simulation::schedule_vehicle_outage(VehicleId id) {
    if (!autonomy_fleet_) throw std::logic_error("vehicle outages require turnaround fleet orchestration");
    const auto found = std::ranges::find(scenario_.vehicles, id, &ServiceVehicle::id);
    if (found == scenario_.vehicles.end()) throw std::invalid_argument("operator outage references unknown vehicle");
    (void)events_.schedule(now_, EventType::OperatorVehicleOutage,
        {EntityKind::Vehicle, id.value()});
}

std::map<EdgeId, VehicleId> Simulation::stationary_ground_vehicle_blockers() const {
    std::map<EdgeId, VehicleId> blockers;
    if (!autonomy_fleet_) return blockers;
    const auto observed = snapshot();
    for (const auto& vehicle_snapshot : observed.vehicles) {
        if (vehicle_snapshot.state != VehicleState::Idle || vehicle_snapshot.fleet_status != "Idle" ||
            !vehicle_snapshot.observed_position_m) continue;
        for (const auto& edge : scenario_.graph.edges()) {
            if (!edge.available || !edge.aircraft_allowed ||
                point_to_edge_distance_m(scenario_.graph, edge, *vehicle_snapshot.observed_position_m) >=
                    surface_safety::aircraft_ground_clearance_m) continue;
            const auto found = blockers.find(edge.id);
            if (found == blockers.end() || vehicle_snapshot.id < found->second)
                blockers[edge.id] = vehicle_snapshot.id;
        }
    }
    return blockers;
}

void Simulation::process(const Event& event) {
    switch (event.type) {
    case EventType::AircraftArrival: handle_aircraft_arrival(AircraftId{event.entity.value}); break;
    case EventType::VehicleArrivalAtAircraft: handle_vehicle_arrival(VehicleId{event.entity.value}); break;
    case EventType::ServiceCompleted: handle_service_completed(VehicleId{event.entity.value}); break;
    case EventType::VehicleArrivalAtDepot: handle_vehicle_return(VehicleId{event.entity.value}); break;
    case EventType::EdgeAvailabilityChanged:
        handle_road_event(EdgeId{event.entity.value}, event.data != 0);
        if (event.data == 0) record_recovery_epoch("surface_closure", std::to_string(event.entity.value));
        break;
    case EventType::AircraftDeparture: handle_departure(AircraftId{event.entity.value}); break;
    case EventType::AbstractServiceCompleted:
        handle_abstract_service_completed(AircraftId{event.entity.value}, TaskId{static_cast<std::uint32_t>(event.data)}); break;
    case EventType::TurnaroundTaskEligible:
        handle_task_eligibility(AircraftId{event.entity.value}, TaskId{static_cast<std::uint32_t>(event.data)}); break;
    case EventType::TaskDurationChanged:
        handle_task_duration_change(TaskId{event.entity.value}, SimTime{event.data});
        record_recovery_epoch("task_duration_change", std::to_string(event.entity.value), SimTime{event.data});
        break;
    case EventType::FleetTick:
        fleet_tick_scheduled_ = false;
        handle_fleet_tick(); break;
    case EventType::VehicleOutage:
        handle_vehicle_outage(VehicleId{event.entity.value});
        record_recovery_epoch("vehicle_outage", std::to_string(event.entity.value));
        break;
    case EventType::OperatorSurfaceAvailability: {
        SimulationEventRecord intervention{SimulationEventType::OperatorIntervention};
        intervention.edge = EdgeId{event.entity.value};
        intervention.detail = std::format("surface_closure target=edge:{} available={} result=accepted",
            event.entity.value, event.data != 0 ? "true" : "false");
        emit(std::move(intervention));
        handle_road_event(EdgeId{event.entity.value}, event.data != 0, true);
        if (event.data == 0) record_recovery_epoch("surface_closure", std::to_string(event.entity.value));
        break;
    }
    case EventType::OperatorVehicleOutage: {
        SimulationEventRecord intervention{SimulationEventType::OperatorIntervention};
        intervention.vehicle = VehicleId{event.entity.value};
        intervention.vehicle_name = vehicle(VehicleId{event.entity.value}).name();
        intervention.detail = std::format("equipment_outage target=vehicle:{} result=accepted", event.entity.value);
        emit(std::move(intervention));
        handle_vehicle_outage(VehicleId{event.entity.value});
        record_recovery_epoch("vehicle_outage", std::to_string(event.entity.value));
        break;
    }
    case EventType::SurfaceTick:
        surface_tick_scheduled_ = false; handle_surface_tick(); break;
    }
}

void Simulation::record_recovery_epoch(std::string trigger, std::string target, SimTime magnitude) {
    revealed_disruptions_.push_back({trigger, std::move(target), now_, magnitude});
    RecoveryState state;
    state.snapshot = snapshot();
    state.revealed_disruptions = revealed_disruptions_;
    const auto fingerprint = recovery_fingerprint(state);
    RecoveryEpochRecord record;
    record.time = now_;
    record.trigger = std::move(trigger);
    record.triage = triage_recovery(state);
    record.fingerprint = fingerprint;
    if (scenario_.recovery_mode == RecoveryMode::Reactive) {
        record.planning_skipped = true;
        record.reason = "reactive_mode";
    } else if (fingerprint == last_recovery_fingerprint_) {
        record.planning_skipped = true;
        record.reason = "equivalent_fingerprint";
    } else {
        // Planning is intentionally not reported as run until an isolated simulator
        // evaluator and authoritative action adapter are available.
        record.planning_skipped = true;
        record.reason = scenario_.recovery_mode == RecoveryMode::Shadow
            ? "shadow_evaluator_not_integrated" : "integrated_action_adapter_not_integrated";
    }
    last_recovery_fingerprint_ = fingerprint;
    recovery_epochs_.push_back(std::move(record));
}

void Simulation::handle_aircraft_arrival(AircraftId id) {
    auto& flight = aircraft(id);
    if (flight.operation_type() != AircraftOperationType::Turnaround) {
        if (!scenario_.surface_operations) throw std::logic_error("inbound arrival requires surface operations");
        flight.begin_surface_arrival();
        auto& state = surface_aircraft_[id];
        state.arrival_operation = true;
        state.phase = SurfaceAircraftState::Phase::ArrivalQueue;
        state.node = scenario_.surface_operations->runway_node;
        state.queued_at = now_;
        state.queue_slot = next_surface_queue_slot_++;
        state.runway_queue_entered_at = now_;
        state.arrival_runway_request_time = now_;
        state.wait_started = now_;
        auto request = aircraft_event(SimulationEventType::RunwayRequest, flight);
        request.detail = "arrival";
        emit(std::move(request));
        emit(aircraft_event(SimulationEventType::SurfaceRunwayQueueEntered, flight));
        if (!surface_tick_scheduled_) {
            surface_tick_scheduled_ = true;
            [[maybe_unused]] const auto sequence = events_.schedule(now_ + SimTime{1}, EventType::SurfaceTick);
        }
        return;
    }
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
    auto route = find_route(scenario_.graph, service_vehicle.current_node(), flight.gate_node(), MovementClass::GroundVehicle);
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
        if (scenario_.surface_operations) request_surface_departure(aircraft_id);
        else [[maybe_unused]] const auto sequence = events_.schedule(
            std::max(now_, flight.scheduled_departure()), EventType::AircraftDeparture,
            {EntityKind::Aircraft, aircraft_id.value()});
    }

    auto return_route = find_route(
        scenario_.graph, flight.gate_node(), service_vehicle.depot_node(), MovementClass::GroundVehicle);
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
        if (scenario_.surface_operations) {
            const bool return_pending = std::ranges::any_of(fleet_return_requests_, [&](const auto& entry) {
                return entry.second.second == flight.id();
            });
            if (!return_pending) request_surface_departure(flight.id());
        }
        else [[maybe_unused]] const auto sequence = events_.schedule(
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

void Simulation::handle_road_event(EdgeId id, bool available, bool operator_triggered) {
    scenario_.graph.set_edge_available(id, available);
    if (autonomy_fleet_) autonomy_fleet_->add_road_event({now_, id, available});
    if (!available) {
        for (auto& [aircraft_id, state] : surface_aircraft_) {
            (void)aircraft_id;
            if (std::ranges::find(state.route_edges, id) != state.route_edges.end())
                state.operator_triggered_invalidation = operator_triggered;
        }
    }
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
    if (!scenario_.turnaround_orchestration || flight.operation_type() == AircraftOperationType::ArrivalOnly ||
        flight.state() == AircraftState::Scheduled || flight.state() == AircraftState::Arriving ||
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

void Simulation::queue_vehicle_return(VehicleId id, NodeId origin, AircraftId aircraft_id, TaskId completed_task) {
    if (!autonomy_fleet_ || !scenario_.surface_operations ||
        !scenario_.surface_operations->return_service_vehicles_to_depot_after_task) return;
    const auto request_id = std::format("ramplab-return-to-depot-{}-{}", id.value(), completed_task.value());
    if (fleet_return_requests_.contains(request_id)) return;
    const auto& service_vehicle = vehicle(id);
    const auto& from = scenario_.graph.node(origin);
    const auto& depot = scenario_.graph.node(service_vehicle.depot_node());
    autonomy::ServiceRequest request;
    request.id = autonomy::ServiceRequestId{request_id};
    request.kind = autonomy::ServiceKind::TugCartMovement;
    request.required_capability = "ramplab_return_" + std::to_string(id.value());
    request.origin = from.name;
    request.destination = depot.name;
    request.release_time_s = autonomy_fleet_->time_s();
    // Dispatch priorities are ordered highest numeric value first; a depot return
    // is part of releasing the just-completed assignment and must preempt queued work.
    request.priority = std::numeric_limits<int>::max();
    request.service_duration_s = 0.0;
    autonomy_fleet_->add_service_request(std::move(request));
    fleet_return_requests_.emplace(request_id, std::pair{id, aircraft_id});
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
    std::string service_staging;
    switch (task_value.type) {
    case ServiceType::Baggage: case ServiceType::BaggageLoad: service_staging = "Baggage Stand"; break;
    case ServiceType::Fueling: service_staging = "Fuel Stand"; break;
    case ServiceType::Catering: service_staging = "Catering Stand"; break;
    case ServiceType::LavatoryService: service_staging = "Lavatory Stand"; break;
    case ServiceType::PotableWater: service_staging = "Water Stand"; break;
    case ServiceType::GroundPower: service_staging = "GPU Stand"; break;
    case ServiceType::Pushback: service_staging = "Pushback Stand"; break;
    case ServiceType::PassengerStairs: service_staging = "Stairs Stand"; break;
    case ServiceType::AirStart: service_staging = "Air Start Stand"; break;
    default: break;
    }
    if (!service_staging.empty()) {
        const auto staging_name = node + " " + service_staging;
        if (std::ranges::any_of(scenario_.graph.nodes(), [&](const auto& candidate) {
                return candidate.name == staging_name;
            })) node = staging_name;
    }
    autonomy::ServiceRequest request;
    request.id = autonomy::ServiceRequestId{request_id};
    request.kind = task_value.type == ServiceType::Fueling
        ? autonomy::ServiceKind::FuelService : autonomy::ServiceKind::BaggageDelivery;
    request.required_capability = fleet_capability(task_value.type);
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
        const bool more_work = autonomy_fleet_->advance();
        // Observe completed service requests before FleetSimulation is allowed to
        // dispatch another task on the next frame. This keeps the vehicle's return
        // request tied to the service bay it actually occupies.
        synchronize_fleet_state();
        if (!more_work) break;
    }
    const auto requests = autonomy_fleet_->service_requests();
    if (!autonomy_fleet_->finished() && std::ranges::any_of(requests, [](const auto& request) {
            return request.state != autonomy::ServiceTaskState::Completed &&
                   request.state != autonomy::ServiceTaskState::Failed;
        })) schedule_fleet_tick();
}

void Simulation::synchronize_fleet_state() {
    if (!autonomy_fleet_) return;
    for (const auto& request : autonomy_fleet_->service_requests()) {
        const auto return_request = fleet_return_requests_.find(request.request.id.value);
        if (return_request != fleet_return_requests_.end()) {
            if (request.state == autonomy::ServiceTaskState::Completed ||
                request.state == autonomy::ServiceTaskState::Failed) {
                const auto aircraft_id = return_request->second.second;
                fleet_return_requests_.erase(return_request);
                const bool another_return_pending = std::ranges::any_of(fleet_return_requests_,
                    [aircraft_id](const auto& entry) { return entry.second.second == aircraft_id; });
                if (request.state == autonomy::ServiceTaskState::Completed && !another_return_pending &&
                    aircraft(aircraft_id).services_complete())
                    request_surface_departure(aircraft_id);
            }
            continue;
        }
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
            if (task_value.status == TaskStatus::Failed || task_value.status == TaskStatus::Completed) break;
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
            if (task_value.status == TaskStatus::Failed) break;
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
                const auto id = task_value.assigned_vehicle;
                if (id && scenario_.surface_operations &&
                    scenario_.surface_operations->return_service_vehicles_to_depot_after_task) {
                    const auto service_location = std::ranges::find(
                        scenario_.graph.nodes(), request.request.destination, &AirportNode::name);
                    if (service_location == scenario_.graph.nodes().end())
                        throw std::logic_error("completed mobile service has an unknown service location");
                    // The mobile vehicle finishes at its assigned service bay. Returning
                    // it from the aircraft's gate center made it retrace through the
                    // stand and could route through another parked GSE position.
                    queue_vehicle_return(*id, service_location->id, flight.id(), task_value.id);
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
                if (const auto surface = surface_aircraft_.find(flight.id()); surface != surface_aircraft_.end()) {
                    surface->second.phase = SurfaceAircraftState::Phase::Failed;
                    surface->second.route_edges.clear();
                    surface->second.route_nodes.clear();
                    std::erase_if(surface_edge_reservations_, [&](const auto& entry) { return entry.second == flight.id(); });
                    std::erase_if(surface_node_reservations_, [&](const auto& entry) { return entry.second == flight.id(); });
                    std::erase_if(gate_arrival_reservations_, [&](const auto& entry) { return entry.second == flight.id(); });
                }
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
    if (!scenario_.turnaround_orchestration || flight.operation_type() == AircraftOperationType::ArrivalOnly ||
        flight.state() == AircraftState::Scheduled || flight.state() == AircraftState::Arriving) return;
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
    if (!scenario_.surface_operations && assigned_gate.occupying_aircraft != id) {
        throw std::logic_error("departing aircraft does not occupy its assigned gate");
    }
    if (assigned_gate.occupying_aircraft == id) assigned_gate.occupying_aircraft.reset();
    emit_aircraft_state(flight, previous);
    emit(aircraft_event(SimulationEventType::AircraftDeparted, flight));
}

void Simulation::complete_arrival_at_gate(AircraftId id) {
    auto& flight = aircraft(id);
    auto& state = surface_aircraft_.at(id);
    auto& assigned_gate = gate(flight.gate());
    if (assigned_gate.occupying_aircraft && assigned_gate.occupying_aircraft != id)
        throw std::logic_error("gate occupancy changed before arrival claim");
    const auto reservation = gate_arrival_reservations_.find(flight.gate());
    if (reservation == gate_arrival_reservations_.end() || reservation->second != id)
        throw std::logic_error("aircraft reached gate without holding its arrival reservation");
    assigned_gate.occupying_aircraft = id;
    if (state.wait_started) state.accumulated_wait += now_ - *state.wait_started;
    state.wait_started.reset();
    state.phase = SurfaceAircraftState::Phase::Arrived;
    state.arrival_gate_at = now_;
    state.arrival_taxi_completed_at = now_;
    complete_surface_agent(id);
    const auto before = flight.state();
    flight.arrive(now_);
    auto at_gate = aircraft_event(SimulationEventType::ArrivalAtGate, flight);
    at_gate.detail = "gate occupied";
    emit(std::move(at_gate));
    emit(aircraft_event(SimulationEventType::AircraftArrived, flight));
    emit_aircraft_state(flight, before);
    if (flight.operation_type() == AircraftOperationType::ArrivalTurnaround) {
        auto started = aircraft_event(SimulationEventType::TurnaroundStarted, flight);
        started.turnaround_id = flight.turnaround_id();
        emit(std::move(started));
        auto created = aircraft_event(SimulationEventType::TurnaroundCreated, flight);
        emit(std::move(created));
        schedule_turnaround_tasks(flight);
    }
}

agents::OperationalAgent& Simulation::surface_agent(AircraftId id, NodeId destination,
    agents::ObjectiveType objective_type) {
    const auto agent_id = std::format("aircraft-{}", id.value());
    const auto objective_id = std::format("{}-{}", agent_id,
        objective_type == agents::ObjectiveType::ReachAssignedStand ? "arrival-stand" : "departure");
    auto [found, inserted] = surface_agents_.try_emplace(id);
    auto& agent = found->second;
    if (inserted) {
        agent.id = agent_id;
        agent.type = agents::AgentType::AircraftSurface;
        agent.status = agents::AgentStatus::Planning;
    }
    agent.objective = {objective_id, objective_type, destination, 0};
    agent.policy_id = std::string(agents::policy_for(scenario_.surface_operations->aircraft_routing_policy).id());
    return agent;
}

agents::Observation Simulation::observe_surface_agent(AircraftId id, NodeId destination,
    bool route_invalidated, std::optional<EdgeId> invalid_edge, bool reservation_blocked,
    bool operator_triggered) const {
    const auto& state = surface_aircraft_.at(id);
    const auto ground_blockers = stationary_ground_vehicle_blockers();
    auto routing_graph = scenario_.graph;
    for (const auto& [edge, vehicle] : ground_blockers) {
        (void)vehicle;
        routing_graph.set_edge_available(edge, false);
    }
    agents::Observation observation;
    observation.agent_id = std::format("aircraft-{}", id.value());
    observation.agent_type = agents::AgentType::AircraftSurface;
    observation.time = now_;
    observation.current_node = state.node;
    observation.destination = destination;
    observation.route_invalidated = route_invalidated;
    observation.invalid_edge = invalid_edge;
    observation.reservation_blocked = reservation_blocked;
    observation.operator_triggered = operator_triggered;
    observation.aircraft_speed_mps = scenario_.surface_operations->aircraft_speed_mps;
    if (state.wait_started) observation.accumulated_wait_age = now_ - *state.wait_started;
    if (reservation_blocked && state.wait_started) observation.blocked_for = now_ - *state.wait_started;
    if (state.edge_index < state.route_edges.size() && state.edge_index < state.route_nodes.size()) {
        Route current;
        current.nodes.assign(state.route_nodes.begin() + static_cast<std::ptrdiff_t>(state.edge_index), state.route_nodes.end());
        current.edges.assign(state.route_edges.begin() + static_cast<std::ptrdiff_t>(state.edge_index), state.route_edges.end());
        for (const auto edge : current.edges) {
            const auto& edge_data = scenario_.graph.edge(edge);
            current.distance_m += edge_data.distance_m;
            current.travel_time += edge_data.traversal_cost;
            observation.current_route_cost += edge_data.traversal_cost;
            const auto reservation = surface_edge_reservations_.find(edge);
            if (reservation != surface_edge_reservations_.end() && reservation->second != id)
                observation.current_route_cost += SimTime{30};
        }
        observation.current_route = std::move(current);
    }
    std::unordered_map<EdgeId, SimTime> expected_wait;
    for (const auto& [edge, owner] : surface_edge_reservations_) {
        if (owner != id) expected_wait.emplace(edge, SimTime{30});
    }
    const bool adaptive_policy = scenario_.surface_operations->aircraft_routing_policy != agents::AutonomyMode::Scripted;
    observation.available_route = find_route(routing_graph, state.node, destination,
        adaptive_policy
            ? expected_wait : std::unordered_map<EdgeId, SimTime>{}, MovementClass::Aircraft);
    if (observation.available_route) observation.available_route_cost = observation.available_route->travel_time;
    std::unordered_map<EdgeId, SimTime> route_waits;
    for (const auto& [edge, owner] : surface_edge_reservations_) {
        if (owner == id) continue;
        SimTime wait{30};
        const auto& owner_state = surface_aircraft_.at(owner);
        if (owner_state.phase_end && *owner_state.phase_end > now_)
            wait = std::max(wait, *owner_state.phase_end - now_);
        route_waits.emplace(edge, wait);
    }
    std::map<AircraftId, std::vector<EdgeId>> shared_intents;
    const bool coordinated_policy = scenario_.surface_operations->aircraft_routing_policy == agents::AutonomyMode::CoordinatedAdaptive;
    const bool cost_aware_policy = scenario_.surface_operations->aircraft_routing_policy == agents::AutonomyMode::CostAwareCounterfactual;
    const bool counterfactual_policy = scenario_.surface_operations->aircraft_routing_policy == agents::AutonomyMode::Counterfactual || cost_aware_policy;
    const bool rolling_policy = scenario_.surface_operations->aircraft_routing_policy == agents::AutonomyMode::RollingHorizonCoordinated || counterfactual_policy;
    const bool shared_policy = coordinated_policy || rolling_policy || counterfactual_policy;
    constexpr std::size_t max_shared_intent_edges = 32U;
    std::size_t shared_intent_edge_count = 0U;
    // Forecast both selected plans and ready pushbacks without plans. This lets
    // the first aircraft to move account for likely routes of other ready traffic.
    for (const auto& [other_id, other_state] : surface_aircraft_) {
        if (other_id == id) continue;
        if (other_state.phase == SurfaceAircraftState::Phase::Failed ||
            other_state.phase == SurfaceAircraftState::Phase::Departed ||
            other_state.phase == SurfaceAircraftState::Phase::Arrived ||
            other_state.phase == SurfaceAircraftState::Phase::WaitingForRunway) continue;
        std::vector<EdgeId> intent_edges;
        const auto other_agent = surface_agents_.find(other_id);
        const auto intent_destination = other_agent != surface_agents_.end()
            ? other_agent->second.objective.destination : scenario_.surface_operations->departure_handoff;
        if (other_agent != surface_agents_.end() && other_agent->second.plan) {
            const auto first_edge = std::min(other_state.edge_index, other_agent->second.plan->edges.size());
            intent_edges.assign(other_agent->second.plan->edges.begin() + static_cast<std::ptrdiff_t>(first_edge),
                other_agent->second.plan->edges.end());
        } else if (shared_policy &&
            other_state.phase == SurfaceAircraftState::Phase::WaitingForPushback) {
            const auto likely_route = find_route(scenario_.graph, other_state.node, intent_destination, MovementClass::Aircraft);
            if (likely_route) intent_edges = likely_route->edges;
        }
        const auto shared_edge_capacity = max_shared_intent_edges - shared_intent_edge_count;
        if (intent_edges.size() > shared_edge_capacity) intent_edges.resize(shared_edge_capacity);
        if (intent_edges.empty()) continue;
        shared_intent_edge_count += intent_edges.size();
        shared_intents.emplace(other_id, intent_edges);
        SimTime eta = other_state.phase == SurfaceAircraftState::Phase::WaitingForPushback
            ? scenario_.surface_operations->pushback_duration : SimTime::zero();
        for (const auto edge : intent_edges) {
            if (shared_policy &&
                observation.shared_traffic.size() < 32U) {
                const auto edge_reservation = surface_edge_reservations_.find(edge);
                SimTime release = now_ + eta + SimTime{30};
                if (edge_reservation != surface_edge_reservations_.end()) {
                    if (other_state.phase_end && *other_state.phase_end > now_) release = *other_state.phase_end;
                }
                const auto traversal = static_cast<std::int64_t>(std::ceil(scenario_.graph.edge(edge).distance_m /
                    std::max(0.1, observation.aircraft_speed_mps)));
                const auto arrival = edge_reservation != surface_edge_reservations_.end() ? now_ : now_ + eta;
                const auto occupancy = edge_reservation != surface_edge_reservations_.end()
                    ? std::max<std::int64_t>(1, (release - now_).count()) : traversal;
                observation.shared_traffic.push_back({std::format("aircraft-{}", other_id.value()), edge,
                    intent_destination, release,
                    other_state.wait_started ? now_ - *other_state.wait_started : SimTime::zero(),
                    other_agent != surface_agents_.end() ? other_agent->second.objective.priority : 0,
                    edge_reservation != surface_edge_reservations_.end(), arrival, SimTime{occupancy}});
            }
            const auto& edge_data = scenario_.graph.edge(edge);
            const auto travel = static_cast<std::int64_t>(std::ceil(edge_data.distance_m /
                std::max(0.1, observation.aircraft_speed_mps)));
            if (!rolling_policy && !surface_edge_reservations_.contains(edge)) route_waits[edge] += SimTime{travel};
            eta += SimTime{travel};
        }
    }
    for (const auto& [node, owner] : surface_node_reservations_) {
        if (owner == id) continue;
        const auto& owner_state = surface_aircraft_.at(owner);
        SimTime wait{30};
        if (owner_state.phase_end && *owner_state.phase_end > now_)
            wait = std::max(wait, *owner_state.phase_end - now_);
        for (const auto edge_id : scenario_.graph.adjacent_edges(state.node)) {
            const auto& edge = scenario_.graph.edge(edge_id);
            if (scenario_.graph.other_endpoint(edge, state.node) == node)
                route_waits[edge_id] = std::max(route_waits[edge_id], wait);
        }
    }
    const auto taxi_seconds = [&](const Route& route) {
        return SimTime{static_cast<std::int64_t>(std::ceil(route.distance_m /
            std::max(0.1, observation.aircraft_speed_mps)))};
    };
    const auto make_candidate = [&](agents::CandidateKind kind, const Route& route) {
        agents::CandidateEvaluation candidate;
        candidate.kind = kind;
        candidate.route = route;
        candidate.predicted_taxi_distance_m = route.distance_m;
        candidate.predicted_taxi_time = taxi_seconds(route);
        for (const auto edge : route.edges) {
            const auto wait = route_waits.find(edge);
            if (wait != route_waits.end()) candidate.expected_wait += wait->second;
        }
        // One second per 50 m gives a small distance preference while predicted
        // time and operational waiting remain the dominant terms.
        candidate.score_seconds = static_cast<double>(candidate.predicted_taxi_time.count() +
            candidate.expected_wait.count()) + candidate.predicted_taxi_distance_m / 50.0;
        candidate.estimated_system_cost_seconds = candidate.score_seconds;
        const auto blocked_edge = std::ranges::find_if(route.edges, [&](EdgeId edge) {
            return ground_blockers.contains(edge);
        });
        if (blocked_edge != route.edges.end()) {
            candidate.valid = false;
            candidate.explanation = std::format("rejected: idle ground vehicle {} occupies the aircraft clearance area of edge {}",
                ground_blockers.at(*blocked_edge).value(), blocked_edge->value());
        }
        if (coordinated_policy) {
            std::map<AircraftId, std::pair<EdgeId, std::int64_t>> conflicting_agents;
            for (const auto& [other_id, intent_edges] : shared_intents) {
                for (const auto shared_edge : intent_edges) {
                    if (std::ranges::find(route.edges, shared_edge) == route.edges.end()) continue;
                    const auto& edge_data = scenario_.graph.edge(shared_edge);
                    const auto traversal = static_cast<std::int64_t>(std::ceil(edge_data.distance_m /
                        std::max(0.1, observation.aircraft_speed_mps)));
                    auto& conflict = conflicting_agents[other_id];
                    if (conflict.second < traversal) conflict = {shared_edge, traversal};
                }
            }
            candidate.affected_agents = conflicting_agents.size();
            if (!conflicting_agents.empty()) {
                const auto& [other_id, conflict] = *conflicting_agents.begin();
                candidate.conflicting_agent = std::format("aircraft-{}", other_id.value());
                candidate.conflicting_edge = conflict.first;
                for (const auto& [other_id_key, conflict_info] : conflicting_agents) {
                    (void)other_id_key;
                    const auto other = surface_agents_.find(other_id_key);
                    const auto& other_state = surface_aircraft_.at(other_id_key);
                    const auto aged_wait = other_state.wait_started
                        ? std::min<std::int64_t>((now_ - *other_state.wait_started).count(), 120) : 0;
                    const auto priority = std::clamp<std::int64_t>(other != surface_agents_.end()
                        ? other->second.objective.priority : 0, 0, 10);
                    const double urgency = 1.0 + static_cast<double>(aged_wait) / 600.0 +
                        static_cast<double>(priority) / 20.0;
                    candidate.estimated_wait_imposed_seconds +=
                        static_cast<double>(conflict_info.second) * urgency;
                }
                candidate.estimated_system_cost_seconds += candidate.estimated_wait_imposed_seconds;
            }
        }
        if (rolling_policy) {
            constexpr std::int64_t horizon_seconds = 600;
            constexpr std::size_t horizon_edges = 8U;
            const auto own_agent = surface_agents_.find(id);
            const auto own_priority = std::clamp<std::int64_t>(own_agent != surface_agents_.end()
                ? own_agent->second.objective.priority : 0, 0, 10);
            const auto own_wait_age = std::min<std::int64_t>(observation.accumulated_wait_age.count(), 120);
            candidate.score_seconds += static_cast<double>(candidate.expected_wait.count()) *
                (static_cast<double>(own_wait_age) / 600.0 + static_cast<double>(own_priority) / 20.0);
            SimTime elapsed{};
            const auto edge_count = std::min(route.edges.size(), horizon_edges);
            std::set<std::string> affected_agents;
            for (std::size_t index = 0; index < edge_count && elapsed.count() < horizon_seconds; ++index) {
                const auto edge_id = route.edges[index];
                const auto travel = static_cast<std::int64_t>(std::ceil(scenario_.graph.edge(edge_id).distance_m /
                    std::max(0.1, observation.aircraft_speed_mps)));
                const SimTime self_arrival = now_ + elapsed;
                const SimTime self_release = self_arrival + SimTime{travel + 10};
                for (const auto& other : observation.shared_traffic) {
                    if (other.edge != edge_id || other.estimated_arrival - now_ > SimTime{horizon_seconds}) continue;
                    const SimTime other_release = other.estimated_arrival + other.estimated_occupancy + SimTime{10};
                    const bool self_arrives_first = self_arrival <= other.estimated_arrival;
                    const SimTime first_release = self_arrives_first ? self_release : other_release;
                    const SimTime second_arrival = self_arrives_first ? other.estimated_arrival : self_arrival;
                    if (second_arrival >= first_release) continue;
                    if (std::ranges::find(candidate.predicted_contention_points, edge_id) ==
                        candidate.predicted_contention_points.end())
                        candidate.predicted_contention_points.push_back(edge_id);
                    const auto conflict_wait = self_arrives_first ? 0LL :
                        std::max<std::int64_t>(0, (other_release - self_arrival).count());
                    const double self_delay = static_cast<double>(conflict_wait);
                    candidate.estimated_self_delay_seconds = std::max(candidate.estimated_self_delay_seconds, self_delay);
                    const auto aged_wait = std::min<std::int64_t>(other.wait_age.count(), 120);
                    const auto priority = std::clamp<std::int64_t>(other.priority, 0, 10);
                    const double urgency = 1.0 + static_cast<double>(aged_wait) / 600.0 +
                        static_cast<double>(priority) / 20.0;
                    const double other_delay = static_cast<double>(travel) * urgency;
                    candidate.estimated_wait_imposed_seconds += other_delay;
                    candidate.predicted_contention.push_back({edge_id, other.agent_id, self_arrival,
                        other.estimated_arrival, self_delay, other_delay});
                    affected_agents.insert(other.agent_id);
                }
                elapsed += SimTime{travel};
            }
            candidate.affected_agents = affected_agents.size();
            candidate.estimated_system_cost_seconds = candidate.score_seconds + candidate.estimated_self_delay_seconds +
                candidate.estimated_wait_imposed_seconds;
            if (!candidate.predicted_contention_points.empty()) {
                candidate.conflicting_edge = candidate.predicted_contention_points.front();
                if (!candidate.predicted_contention.empty())
                    candidate.conflicting_agent = candidate.predicted_contention.front().agent_id;
            }
            candidate.explanation = "bounded 600-second / 8-edge arrival-window forecast + taxi, priority and wait-age weighted reservation wait, distance and estimated fleet delay";
        }
        if (!rolling_policy) candidate.explanation = "taxi_time + expected_reservation_wait + distance_m/50";
        if (coordinated_policy)
            candidate.explanation += " + edge occupancy imposed on active planned and ready route intents";
        return candidate;
    };
    if (!observation.objective_satisfied) {
        const auto alternatives = find_route_candidates(routing_graph, state.node, destination, 6U, route_waits, MovementClass::Aircraft);
        if (observation.current_route) {
            auto candidate = make_candidate(agents::CandidateKind::ContinueCurrentRoute,
                *observation.current_route);
            if (route_invalidated) {
                candidate.valid = false;
                candidate.explanation = "rejected: current route was invalidated by a closure or route change";
            }
            observation.candidates.push_back(std::move(candidate));
        }
        for (const auto& route : alternatives) {
            if (observation.current_route && route.edges == observation.current_route->edges) continue;
            observation.candidates.push_back(make_candidate(agents::CandidateKind::AlternateRoute, route));
        }
        if (reservation_blocked) {
            SimTime wait{30};
            if (observation.current_route) {
                for (const auto edge : observation.current_route->edges) {
                    const auto estimate = route_waits.find(edge);
                    if (estimate != route_waits.end()) wait = std::max(wait, estimate->second);
                }
            }
            agents::CandidateEvaluation candidate;
            candidate.kind = agents::CandidateKind::Wait;
            candidate.route = observation.current_route;
            candidate.expected_wait = wait;
            if (observation.current_route) {
                candidate.predicted_taxi_distance_m = observation.current_route->distance_m;
                candidate.predicted_taxi_time = taxi_seconds(*observation.current_route);
            }
            candidate.score_seconds = static_cast<double>(wait.count() + candidate.predicted_taxi_time.count()) +
                candidate.predicted_taxi_distance_m / 50.0;
            if (coordinated_policy || rolling_policy) {
                candidate.estimated_system_cost_seconds = candidate.score_seconds;
                if (rolling_policy) {
                    const auto own_agent = surface_agents_.find(id);
                    const auto own_priority = std::clamp<std::int64_t>(own_agent != surface_agents_.end()
                        ? own_agent->second.objective.priority : 0, 0, 10);
                    const auto own_wait_age = std::min<std::int64_t>(observation.accumulated_wait_age.count(), 120);
                    candidate.score_seconds += static_cast<double>(candidate.expected_wait.count()) *
                        (static_cast<double>(own_wait_age) / 600.0 + static_cast<double>(own_priority) / 20.0);
                    candidate.estimated_system_cost_seconds = candidate.score_seconds;
                    candidate.explanation = "wait estimate + remaining current-route taxi time + priority and wait-age weighted reservation wait";
                }
            }
            candidate.explanation = "wait estimate + remaining current-route taxi time + distance_m/50";
            observation.candidates.push_back(std::move(candidate));
        }
    }
    if (counterfactual_policy) {
        // Private event rollout: current safe route candidates × 600 simulated
        // seconds, at most eight edges and 256 deterministic edge/conflict events
        // per candidate. Only the observation's present reservations, published
        // routes and already-revealed graph availability enter this snapshot.
        constexpr std::int64_t rollout_horizon_seconds = 600;
        constexpr std::size_t rollout_edge_limit = 8U;
        constexpr std::size_t rollout_event_limit = 256U;
        const auto rollout_graph = scenario_.graph; // isolated copy of current revealed graph state
        const auto rollout_traffic = observation.shared_traffic; // current reservations and published/observed intents only
        const auto rollout_now = now_;
        const auto rollout_speed = observation.aircraft_speed_mps;
        SimTime observed_hold{};
        if (observation.reservation_blocked) {
            const auto hold = std::ranges::find(observation.candidates, agents::CandidateKind::Wait,
                &agents::CandidateEvaluation::kind);
            if (hold != observation.candidates.end()) observed_hold = hold->expected_wait;
        }
        for (auto& candidate : observation.candidates) {
            if (!candidate.valid || !candidate.route) continue;
            auto& result = candidate;
            result.counterfactual_evaluated = true;
            const SimTime initial_wait = result.kind == agents::CandidateKind::Wait ? result.expected_wait :
                result.kind == agents::CandidateKind::ContinueCurrentRoute ? observed_hold : SimTime::zero();
            SimTime elapsed = initial_wait;
            SimTime downstream_wait{};
            SimTime taxi_time{};
            std::size_t events = 0U;
            std::size_t processed_edges = 0U;
            std::size_t contention_count = 0U;
            double contention_delay = 0.0;
            downstream_wait = initial_wait;
            bool reached = true;
            const auto count = std::min(result.route->edges.size(), rollout_edge_limit);
            if (result.route->edges.size() > count) reached = false;
            for (std::size_t edge_index = 0; edge_index < count && events < rollout_event_limit &&
                elapsed.count() < rollout_horizon_seconds; ++edge_index) {
                const auto edge_id = result.route->edges[edge_index];
                const auto& edge = rollout_graph.edge(edge_id);
                const auto seconds = std::max<std::int64_t>(1, static_cast<std::int64_t>(std::ceil(
                    edge.distance_m / std::max(0.1, rollout_speed))));
                const SimTime travel{seconds};
                const SimTime own_arrival = rollout_now + elapsed;
                const SimTime own_release = own_arrival + travel + SimTime{10};
                SimTime blocking_release = own_arrival;
                SimTime other_wait{};
                ++events; // private taxi edge-entry event
                for (const auto& other : rollout_traffic) {
                    if (events >= rollout_event_limit) break;
                    if (other.edge != edge_id || other.estimated_arrival - now_ > SimTime{rollout_horizon_seconds}) continue;
                    ++events; // private reservation/intent conflict event
                    const SimTime other_release = other.estimated_arrival + other.estimated_occupancy + SimTime{10};
                    if (own_arrival >= other_release || other.estimated_arrival >= own_release) continue;
                    ++contention_count;
                    const SimTime overlap = std::max(SimTime::zero(), std::min(own_release, other_release) -
                        std::max(own_arrival, other.estimated_arrival));
                    contention_delay += static_cast<double>(overlap.count());
                    if (other.estimated_arrival <= own_arrival) {
                        blocking_release = std::max(blocking_release, other_release);
                    } else {
                        const auto imposed = std::max(SimTime::zero(), own_release - other.estimated_arrival);
                        other_wait += imposed;
                    }
                }
                const auto own_wait = std::max(SimTime::zero(), blocking_release - own_arrival);
                downstream_wait += own_wait + other_wait;
                elapsed += own_wait;
                elapsed += travel;
                taxi_time += travel;
                ++events; // private taxi edge-release event
                ++processed_edges;
            }
            if (elapsed.count() > rollout_horizon_seconds || count < result.route->edges.size() || processed_edges < count) reached = false;
            result.rollout_completed_objectives = reached ? 1U : 0U;
            result.rollout_blocked_objectives = reached ? 0U : 1U;
            result.rollout_wait = downstream_wait;
            result.rollout_taxi_time = taxi_time;
            result.rollout_completion_time = elapsed;
            result.rollout_distance_m = result.route->distance_m;
            result.rollout_events = events;
            result.rollout_contention_count = contention_count;
            result.rollout_contention_delay_seconds = contention_delay;
            result.explanation = "isolated current-observation edge-event rollout; 600 s horizon, 8 edges, 256 events; future unrevealed disruptions excluded";
        }
        if (cost_aware_policy) {
            const auto own_agent = surface_agents_.find(id);
            const agents::Objective objective = own_agent != surface_agents_.end()
                ? own_agent->second.objective : agents::Objective{"", agents::ObjectiveType::ReachDestination, destination, 0};
            agents::evaluate_operational_costs(observation, objective);
        }
    }
    observation.objective_satisfied = state.node == destination;
    return observation;
}

void Simulation::record_agent_decision(AircraftId id, const agents::Observation& observation,
    const agents::Decision& decision, std::string result) {
    auto& agent = surface_agents_.at(id);
    const bool cost_aware_policy = scenario_.surface_operations->aircraft_routing_policy ==
        agents::AutonomyMode::CostAwareCounterfactual;
    agents::DecisionEvent trace;
    trace.event_id = std::format("agent-decision-{}", agent_decisions_.size());
    trace.time = now_;
    trace.agent_id = agent.id;
    trace.agent_type = agent.type;
    trace.policy_id = agent.policy_id;
    trace.objective = agent.objective;
    trace.observation = observation;
    trace.candidates = observation.candidates;
    if (decision.action == agents::ActionType::Wait) {
        const auto selected = std::ranges::find(trace.candidates, agents::CandidateKind::Wait,
            &agents::CandidateEvaluation::kind);
        if (selected != trace.candidates.end()) trace.selected_candidate =
            static_cast<std::size_t>(std::distance(trace.candidates.begin(), selected));
    } else if (decision.route) {
        const auto selected = std::ranges::find_if(trace.candidates, [&](const auto& candidate) {
            return candidate.route && candidate.route->edges == decision.route->edges;
        });
        if (selected != trace.candidates.end()) trace.selected_candidate =
            static_cast<std::size_t>(std::distance(trace.candidates.begin(), selected));
    }
    trace.action = decision.action;
    trace.status = decision.status;
    trace.reason = decision.reason;
    trace.observed_node = observation.current_node;
    trace.destination = observation.destination;
    trace.invalid_edge = observation.invalid_edge;
    trace.previous_route = observation.current_route;
    trace.selected_route = decision.route;
    trace.operator_triggered = observation.operator_triggered;
    trace.result = std::move(result);
    trace.explanation = decision.explanation;
    if (trace.selected_candidate) {
        const auto& selected = trace.candidates[*trace.selected_candidate];
        trace.estimated_system_cost_seconds = selected.estimated_system_cost_seconds;
        trace.affected_agents = selected.affected_agents;
        trace.coordination_conflict_agent = selected.conflicting_agent;
        trace.coordination_conflict_edge = selected.conflicting_edge;
        if (scenario_.surface_operations->aircraft_routing_policy == agents::AutonomyMode::CoordinatedAdaptive)
            trace.coordination_rule = "minimize local taxi cost plus estimated shared-route delay; stable candidate order; movement contention prioritizes wait age, objective priority, then aircraft ID";
        if (scenario_.surface_operations->aircraft_routing_policy == agents::AutonomyMode::CoordinatedAdaptive)
            trace.safety_constraints = "closed-edge rejection; exclusive edge and node reservations; aircraft and ground-vehicle clearance checks; safe failure on no feasible route";
    }
    if (scenario_.surface_operations->aircraft_routing_policy == agents::AutonomyMode::RollingHorizonCoordinated) {
        trace.coordination_rule = "minimize taxi, reservation wait, and time-window overlap cost over at most 600 seconds and 8 route edges; ties preserve enumerated route order";
        trace.planning_horizon = SimTime{600};
        trace.tie_break_reason = "equal forecast costs retain the stable route candidate enumeration order";
        if (decision.action == agents::ActionType::PlanRoute) {
            if (observation.operator_triggered) trace.replan_trigger = "operator intervention";
            else if (observation.invalid_edge) trace.replan_trigger = "route closure or reopening invalidated the route";
            else if (observation.route_invalidated) trace.replan_trigger = "route invalidation";
            else if (decision.reason == agents::ReasonCode::DestinationChanged) trace.replan_trigger = "objective destination changed";
            else if (observation.reservation_blocked) trace.replan_trigger = "reservation contention changed the feasible forecast";
            else if (!observation.current_route) trace.replan_trigger = "new objective or missing route";
            else trace.replan_trigger = "bounded forecast improvement exceeded threshold";
        } else if (decision.action == agents::ActionType::Wait) {
            trace.replan_trigger = "none; waiting for authoritative reservation release";
        } else if (decision.action == agents::ActionType::CompleteObjective) {
            trace.replan_trigger = "none; objective completed";
        } else {
            trace.replan_trigger = "none; current route retained";
        }
        trace.safety_constraints = "closed-edge rejection; exclusive edge and node reservations; aircraft and ground-vehicle clearance checks; safe failure on no feasible route";
    }
    if (scenario_.surface_operations->aircraft_routing_policy == agents::AutonomyMode::Counterfactual ||
        scenario_.surface_operations->aircraft_routing_policy == agents::AutonomyMode::CostAwareCounterfactual) {
        trace.planning_horizon = SimTime{600};
        trace.coordination_rule = cost_aware_policy
            ? "Goal 40 first preserves best bounded-rollout objective completion; then normalized cost weights: wait .40, completion time .30, distance .18, contention .08, route change .04; material improvement >= .01 and non-wait regret <= 0.85 of waiting benefit; wait-age and priority raise wait weight"
            : "lexicographic: completed objectives, blocked objectives, downstream wait, taxi time, taxi distance, stable candidate order";
        trace.tie_break_reason = cost_aware_policy
            ? "lower normalized total operational cost wins; exact cost ties retain stable candidate enumeration order; baseline retained when thresholds fail"
            : "candidate enumeration order is stable; first candidate is retained on a complete lexicographic tie";
        trace.replan_trigger = observation.operator_triggered ? "operator intervention" :
            observation.invalid_edge ? "route invalidation" : observation.reservation_blocked ? "reservation blocked" :
            observation.current_route ? "eligible route comparison" : "new objective or missing route";
        trace.counterfactual_rollouts = static_cast<std::size_t>(std::ranges::count_if(trace.candidates,
            [](const auto& candidate) { return candidate.counterfactual_evaluated; }));
        trace.counterfactual_events = 0U;
        for (const auto& candidate : trace.candidates) trace.counterfactual_events += candidate.rollout_events;
        const auto rolling = agents::RollingHorizonCoordinatedPolicy{}.decide(observation, agent.objective, agent.memory);
        trace.differs_from_rolling_horizon = rolling.action != decision.action || rolling.route != decision.route;
        trace.objective_priority = agent.objective.priority;
        trace.wait_age = observation.accumulated_wait_age;
        trace.cost_aware_evaluated = cost_aware_policy;
        trace.operational_cost_config = agents::OperationalCostConfig{};
        trace.state_identifier = std::format("{}@{}:{}>{}:{}", observation.agent_id,
            observation.time.count(), observation.current_node.value(), observation.destination.value(),
            agents::route_string(observation.current_route));
        trace.safety_constraints = "route candidates were prefiltered by live graph availability; authoritative edge and node reservations, clearance and collision checks still gate execution; no safety term is traded for score";
    }
    agent.status = decision.status;
    agent.current_action = decision.action;
    agent.last_reason = decision.reason;
    agent.history.push_back(trace);
    agent_decisions_.push_back(trace);

    SimulationEventRecord event{SimulationEventType::AgentDecision};
    event.aircraft = id;
    event.agent_id = trace.agent_id;
    event.agent_event_id = trace.event_id;
    event.agent_type = std::string(agents::to_string(trace.agent_type));
    event.policy_id = trace.policy_id;
    event.objective_id = trace.objective.id;
    event.decision = std::string(agents::to_string(trace.action));
    event.reason_code = std::string(agents::to_string(trace.reason));
    event.route = trace.selected_route;
    event.previous_route = trace.previous_route;
    event.observation_summary = std::format("node={} destination={} blocked={} operator={}",
        observation.current_node.value(), observation.destination.value(),
        observation.reservation_blocked ? "true" : "false", observation.operator_triggered ? "true" : "false");
    event.decision_result = trace.result;
    event.detail = decision.explanation + " result=" + trace.result;
    emit(std::move(event));
}

agents::Decision Simulation::decide_surface_agent(AircraftId id, NodeId destination,
    agents::ObjectiveType objective_type, bool route_invalidated, std::optional<EdgeId> invalid_edge,
    bool reservation_blocked, bool operator_triggered) {
    auto& agent = surface_agent(id, destination, objective_type);
    auto observation = observe_surface_agent(id, destination, route_invalidated, invalid_edge,
        reservation_blocked, operator_triggered);
    auto decision = agents::policy_for(scenario_.surface_operations->aircraft_routing_policy)
        .decide(observation, agent.objective, agent.memory);
    ++agent.memory.decisions;
    agent.memory.last_destination = destination;
    if (decision.action == agents::ActionType::PlanRoute || decision.action == agents::ActionType::CompleteObjective ||
        decision.action == agents::ActionType::FailObjective) {
        agent.memory.last_replan_at = now_;
        if (observation.current_route) agent.memory.previous_route = observation.current_route->nodes;
    }
    std::string result = decision.action == agents::ActionType::PlanRoute ? "ROUTE_SELECTED" :
        decision.action == agents::ActionType::FailObjective ? "OBJECTIVE_FAILED" :
        decision.action == agents::ActionType::CompleteObjective ? "OBJECTIVE_COMPLETED" :
        decision.action == agents::ActionType::Wait ? "WAITING" : "CONTINUING";
    record_agent_decision(id, observation, decision, std::move(result));
    return decision;
}

void Simulation::apply_surface_agent_route(AircraftId id, NodeId destination, const agents::Decision& decision) {
    auto& state = surface_aircraft_.at(id);
    if (decision.action == agents::ActionType::PlanRoute && decision.route) {
        state.route_nodes = decision.route->nodes;
        state.route_edges = decision.route->edges;
        state.edge_index = 0;
        surface_agents_.at(id).plan = decision.route;
        return;
    }
    if (decision.action == agents::ActionType::FailObjective) {
        state.phase = SurfaceAircraftState::Phase::Failed;
        surface_agents_.at(id).objective.destination = destination;
    }
}

void Simulation::complete_surface_agent(AircraftId id) {
    auto found = surface_agents_.find(id);
    if (found == surface_agents_.end() || found->second.status == agents::AgentStatus::Completed) return;
    const auto destination = found->second.objective.destination;
    auto& state = surface_aircraft_.at(id);
    const bool at_destination = state.node == destination;
    if (!at_destination) return;
    const auto decision = decide_surface_agent(id, destination, found->second.objective.type);
    (void)decision;
}

void Simulation::start_arrival_taxi_in(AircraftId id) {
    auto& flight = aircraft(id);
    auto& state = surface_aircraft_.at(id);
    auto& assigned_gate = gate(flight.gate());
    const auto reserved = gate_arrival_reservations_.find(flight.gate());
    if (!assigned_gate.enabled || assigned_gate.occupying_aircraft ||
        (reserved != gate_arrival_reservations_.end() && reserved->second != id)) {
        state.phase = SurfaceAircraftState::Phase::WaitingForGate;
        if (!state.gate_wait_started_at) state.gate_wait_started_at = now_;
        if (!state.wait_started) {
            state.wait_started = now_;
            ++surface_wait_events_;
            emit(aircraft_event(SimulationEventType::GateWaitStarted, flight));
        }
        return;
    }
    gate_arrival_reservations_[flight.gate()] = id;
    state.gate_assigned_at = now_;
    auto gate_assigned = aircraft_event(SimulationEventType::GateAssigned, flight);
    gate_assigned.detail = "arrival gate reserved";
    emit(std::move(gate_assigned));
    const auto decision = decide_surface_agent(id, flight.gate_node(), agents::ObjectiveType::ReachAssignedStand);
    apply_surface_agent_route(id, flight.gate_node(), decision);
    if (decision.action != agents::ActionType::PlanRoute || !decision.route) {
        gate_arrival_reservations_.erase(flight.gate());
        surface_node_reservations_.erase(state.node);
        flight.fail_turnaround("no available taxi route from runway exit to assigned arrival gate");
        auto failure = aircraft_event(SimulationEventType::SurfaceSafeFailure, flight);
        failure.detail = flight.failure_reason();
        emit(std::move(failure));
        return;
    }
    if (state.wait_started) state.accumulated_wait += now_ - *state.wait_started;
    state.wait_started.reset();
    if (state.gate_wait_started_at) {
        state.gate_wait_duration += now_ - *state.gate_wait_started_at;
        state.gate_wait_started_at.reset();
    }
    state.phase = SurfaceAircraftState::Phase::Taxiing;
    state.taxi_started_at = now_;
    state.taxi_completed_at.reset();
    state.arrival_taxi_started_at = now_;
    state.arrival_taxi_completed_at.reset();
    emit(aircraft_event(SimulationEventType::ArrivalTaxiInStarted, flight));
    auto assigned = aircraft_event(SimulationEventType::SurfaceTaxiRouteAssigned, flight);
    assigned.route = *decision.route;
    emit(std::move(assigned));
}

void Simulation::request_surface_departure(AircraftId id) {
    auto& flight = aircraft(id);
    auto& state = surface_aircraft_[id];
    if (state.phase != SurfaceAircraftState::Phase::None &&
        !(state.phase == SurfaceAircraftState::Phase::Arrived && flight.operation_type() == AircraftOperationType::ArrivalTurnaround)) return;
    state.arrival_operation = false;
    state.node = flight.gate_node();
    state.phase = SurfaceAircraftState::Phase::WaitingForPushback;
    state.wait_started = now_;
    const auto destination = scenario_.surface_operations->departure_handoff;
    const auto decision = decide_surface_agent(id, destination, agents::ObjectiveType::DepartAirport);
    apply_surface_agent_route(id, destination, decision);
    if (decision.action != agents::ActionType::PlanRoute || !decision.route)
        state.phase = SurfaceAircraftState::Phase::Failed;
    auto requested = aircraft_event(SimulationEventType::SurfacePushbackRequested, flight);
    emit(std::move(requested));
    if (state.phase == SurfaceAircraftState::Phase::Failed)
        emit(aircraft_event(SimulationEventType::SurfaceSafeFailure, flight));
    if (!surface_tick_scheduled_) {
        surface_tick_scheduled_ = true;
        [[maybe_unused]] const auto sequence = events_.schedule(now_ + SimTime{1}, EventType::SurfaceTick);
    }
}

void Simulation::finish_surface_departure(AircraftId id) {
    auto& state = surface_aircraft_.at(id);
    state.phase = SurfaceAircraftState::Phase::Departed;
    state.phase_end.reset();
    state.actual_surface_departure = now_;
    state.runway_release_at = now_;
    state.departure_runway_release_time = now_;
    if (state.departure_runway_clearance_time)
        state.departure_runway_occupied += now_ - *state.departure_runway_clearance_time;
    if (state.phase_started) state.runway_occupied += now_ - *state.phase_started;
    handle_departure(id);
}

void Simulation::handle_surface_tick() {
    using Phase = SurfaceAircraftState::Phase;
    const auto config = *scenario_.surface_operations;
    const auto event_for = [this](SimulationEventType type, AircraftId id, std::optional<EdgeId> edge = std::nullopt) {
        auto event = aircraft_event(type, aircraft(id));
        event.edge = edge;
        if (type == SimulationEventType::SurfaceRunwayQueueEntered) {
            auto request = aircraft_event(SimulationEventType::RunwayRequest, aircraft(id));
            request.detail = surface_aircraft_.at(id).arrival_operation ? "arrival" : "departure";
            emit(std::move(request));
        }
        emit(std::move(event));
    };

    // Complete pushback, committed edge traversals, and runway occupancy first.
    for (auto& [id, state] : surface_aircraft_) {
        if (!state.phase_end || *state.phase_end > now_) continue;
        if (state.phase == Phase::Pushback) {
            state.phase = Phase::Taxiing;
            state.pushback_completed_at = now_;
            state.departure_taxi_started_at = now_;
            state.phase_end.reset();
            state.phase_started.reset();
            auto& assigned_gate = gate(aircraft(id).gate());
            if (assigned_gate.occupying_aircraft == id) assigned_gate.occupying_aircraft.reset();
            if (!state.route_edges.empty()) {
                surface_edge_reservations_.erase(state.route_edges.front());
                surface_node_reservations_.erase(state.route_nodes[1]);
                event_for(SimulationEventType::SurfaceReservationReleased, id, state.route_edges.front());
            }
            surface_node_reservations_[state.node] = id;
            event_for(SimulationEventType::SurfacePushbackCompleted, id);
            state.route_nodes.clear();
            state.route_edges.clear();
            state.edge_index = 0;
            const auto decision = decide_surface_agent(id, config.departure_handoff, agents::ObjectiveType::DepartAirport);
            apply_surface_agent_route(id, config.departure_handoff, decision);
            if (decision.action != agents::ActionType::PlanRoute || !decision.route) {
                event_for(SimulationEventType::SurfaceSafeFailure, id);
                continue;
            }
            auto route_event = aircraft_event(SimulationEventType::SurfaceTaxiRouteAssigned, aircraft(id));
            route_event.route = *decision.route;
            emit(std::move(route_event));
            if (state.route_edges.empty()) {
                surface_node_reservations_.erase(state.node);
                state.phase = Phase::WaitingForRunway;
                state.queued_at = now_;
                state.queue_slot = next_surface_queue_slot_++;
                state.runway_queue_entered_at = now_;
                state.departure_runway_request_time = now_;
                state.wait_started = now_;
                event_for(SimulationEventType::SurfaceRunwayQueueEntered, id);
            }
        } else if (state.phase == Phase::Taxiing && state.edge_index < state.route_edges.size()) {
            const auto edge_id = state.route_edges[state.edge_index];
            if (surface_edge_reservations_.contains(edge_id)) surface_edge_reservations_.erase(edge_id);
            event_for(SimulationEventType::SurfaceReservationReleased, id, edge_id);
            state.node = state.route_nodes[state.edge_index + 1];
            state.edge_index++;
            state.phase_end.reset();
            state.phase_started.reset();
            if (state.edge_index >= state.route_edges.size()) {
                surface_node_reservations_.erase(state.node);
                state.taxi_completed_at = now_;
                if (state.arrival_operation) {
                    state.arrival_taxi_completed_at = now_;
                    auto& assigned_gate = gate(aircraft(id).gate());
                    if (assigned_gate.occupying_aircraft && assigned_gate.occupying_aircraft != id) {
                        state.phase = Phase::Failed;
                        gate_arrival_reservations_.erase(aircraft(id).gate());
                        event_for(SimulationEventType::SurfaceSafeFailure, id);
                        aircraft(id).fail_turnaround("assigned gate became occupied despite arrival reservation");
                    } else {
                        complete_arrival_at_gate(id);
                    }
                    continue;
                }
                state.departure_taxi_completed_at = now_;
                complete_surface_agent(id);
                state.phase = Phase::WaitingForRunway;
                state.queued_at = now_;
                state.queue_slot = next_surface_queue_slot_++;
                state.runway_queue_entered_at = now_;
                state.departure_runway_request_time = now_;
                state.wait_started = now_;
                event_for(SimulationEventType::SurfaceRunwayQueueEntered, id);
            }
        } else if (state.phase == Phase::Runway) {
            if (state.arrival_operation) {
                const auto exit_node = aircraft(id).arrival_exit_node();
                const auto occupied = surface_node_reservations_.find(exit_node);
                if (occupied != surface_node_reservations_.end() && occupied->second != id) {
                    state.phase_end = now_ + SimTime{1};
                    continue;
                }
                state.phase = Phase::Taxiing;
                state.node = exit_node;
                state.phase_end.reset();
                if (state.phase_started) state.runway_occupied += now_ - *state.phase_started;
                state.phase_started.reset();
                surface_node_reservations_[exit_node] = id;
                auto release = aircraft_event(SimulationEventType::RunwayReleased, aircraft(id));
                release.detail = "arrival rollout complete";
                emit(std::move(release));
                state.runway_release_at = now_;
                state.arrival_runway_release_time = now_;
                if (state.arrival_runway_clearance_time)
                    state.arrival_runway_occupied += now_ - *state.arrival_runway_clearance_time;
                auto runway_exit = aircraft_event(SimulationEventType::ArrivalRunwayExit, aircraft(id));
                runway_exit.detail = "runway exit " + std::to_string(exit_node.value());
                emit(std::move(runway_exit));
                start_arrival_taxi_in(id);
            } else {
                auto release = aircraft_event(SimulationEventType::RunwayReleased, aircraft(id));
                release.detail = "departure interval complete";
                emit(std::move(release));
                finish_surface_departure(id);
            }
        }
    }

    const auto ground_blockers = stationary_ground_vehicle_blockers();
    std::vector<AircraftId> movement_order;
    movement_order.reserve(surface_aircraft_.size());
    for (const auto& [id, state] : surface_aircraft_) {
        (void)state;
        movement_order.push_back(id);
    }
    if (config.aircraft_routing_policy == agents::AutonomyMode::CoordinatedAdaptive ||
        config.aircraft_routing_policy == agents::AutonomyMode::RollingHorizonCoordinated ||
        config.aircraft_routing_policy == agents::AutonomyMode::CostAwareCounterfactual) {
        std::ranges::stable_sort(movement_order, [&](AircraftId left, AircraftId right) {
            const auto& lhs = surface_aircraft_.at(left);
            const auto& rhs = surface_aircraft_.at(right);
            const auto lhs_age = lhs.wait_started ? now_ - *lhs.wait_started : SimTime::zero();
            const auto rhs_age = rhs.wait_started ? now_ - *rhs.wait_started : SimTime::zero();
            const auto lhs_agent = surface_agents_.find(left);
            const auto rhs_agent = surface_agents_.find(right);
            const auto lhs_priority = lhs_agent == surface_agents_.end() ? 0 : lhs_agent->second.objective.priority;
            const auto rhs_priority = rhs_agent == surface_agents_.end() ? 0 : rhs_agent->second.objective.priority;
            return agents::coordination_precedes({lhs_age, lhs_priority, left.value()},
                {rhs_age, rhs_priority, right.value()});
        });
    }
    for (const auto id : movement_order) {
        auto& state = surface_aircraft_.at(id);
        if (state.phase == Phase::WaitingForGate) {
            const auto& assigned_gate = gate(aircraft(id).gate());
            const auto reservation = gate_arrival_reservations_.find(aircraft(id).gate());
            if (assigned_gate.enabled && !assigned_gate.occupying_aircraft &&
                (reservation == gate_arrival_reservations_.end() || reservation->second == id))
                start_arrival_taxi_in(id);
            continue;
        }
        if (state.phase == Phase::WaitingForPushback) {
            if (state.route_edges.empty()) {
                state.phase = Phase::WaitingForRunway;
                state.queued_at = now_;
                state.queue_slot = next_surface_queue_slot_++;
                state.runway_queue_entered_at = now_;
                state.departure_runway_request_time = now_;
                state.wait_started = now_;
                event_for(SimulationEventType::SurfaceRunwayQueueEntered, id);
                continue;
            }
            const auto edge_id = state.route_edges.front();
            const auto ground_blocker = ground_blockers.find(edge_id);
            const bool blocked_by_stationary_ground_vehicle = ground_blocker != ground_blockers.end();
            if (!scenario_.graph.edge(edge_id).available) {
                event_for(SimulationEventType::SurfaceRouteInvalidated, id, edge_id);
                const auto destination = state.arrival_operation ? aircraft(id).gate_node() : config.departure_handoff;
                const auto objective = state.arrival_operation ? agents::ObjectiveType::ReachAssignedStand : agents::ObjectiveType::DepartAirport;
                const auto decision = decide_surface_agent(id, destination, objective, true, edge_id,
                    false, state.operator_triggered_invalidation);
                state.operator_triggered_invalidation = false;
                apply_surface_agent_route(id, destination, decision);
                if (decision.action != agents::ActionType::PlanRoute || !decision.route) {
                    event_for(SimulationEventType::SurfaceSafeFailure, id);
                    continue;
                }
                ++state.reroutes;
                auto rerouted = aircraft_event(SimulationEventType::SurfaceRerouted, aircraft(id));
                rerouted.route = *decision.route;
                emit(std::move(rerouted));
            }
            if (state.route_edges.empty()) continue;
            const auto selected_edge = state.route_edges.front();
            const auto selected_node = state.route_nodes[1];
            const auto edge_owner = surface_edge_reservations_.find(selected_edge);
            const auto node_owner = surface_node_reservations_.find(selected_node);
            if (edge_owner != surface_edge_reservations_.end() || node_owner != surface_node_reservations_.end() ||
                blocked_by_stationary_ground_vehicle) {
                if (!state.pushback_wait_reported) {
                    state.pushback_wait_reported = true;
                    ++surface_wait_events_;
                    auto waiting = aircraft_event(SimulationEventType::SurfaceWaitingForPushback, aircraft(id));
                    if (blocked_by_stationary_ground_vehicle) {
                        waiting.vehicle = ground_blocker->second;
                        waiting.edge = edge_id;
                        waiting.detail = "idle ground vehicle occupies the aircraft clearance area of the pushback edge";
                    }
                    emit(std::move(waiting));
                }
                const auto agent = surface_agents_.find(id);
                if ((blocked_by_stationary_ground_vehicle ||
                     config.aircraft_routing_policy != agents::AutonomyMode::Scripted) && state.wait_started &&
                    (blocked_by_stationary_ground_vehicle || agent == surface_agents_.end() || !agent->second.memory.last_blockage_decision_at ||
                     now_ - *agent->second.memory.last_blockage_decision_at >= SimTime{30}) &&
                    (blocked_by_stationary_ground_vehicle || now_ - *state.wait_started >= SimTime{30})) {
                    surface_agents_.at(id).memory.last_blockage_decision_at = now_;
                    const auto decision = decide_surface_agent(id, config.departure_handoff,
                        agents::ObjectiveType::DepartAirport, blocked_by_stationary_ground_vehicle,
                        std::nullopt, !blocked_by_stationary_ground_vehicle);
                    if (decision.action == agents::ActionType::PlanRoute && decision.route) {
                        apply_surface_agent_route(id, config.departure_handoff, decision);
                        ++state.reroutes;
                        auto rerouted = aircraft_event(SimulationEventType::SurfaceRerouted, aircraft(id));
                        rerouted.route = *decision.route;
                        emit(std::move(rerouted));
                        continue;
                    }
                    if (decision.action == agents::ActionType::FailObjective) {
                        apply_surface_agent_route(id, config.departure_handoff, decision);
                        auto failure = aircraft_event(SimulationEventType::SurfaceSafeFailure, aircraft(id));
                        failure.edge = edge_id;
                        if (blocked_by_stationary_ground_vehicle) failure.vehicle = ground_blocker->second;
                        failure.detail = decision.explanation;
                        emit(std::move(failure));
                        continue;
                    }
                }
                continue;
            }
            if (!scenario_.graph.edge(selected_edge).available ||
                (edge_owner != surface_edge_reservations_.end() && edge_owner->second != id) ||
                (node_owner != surface_node_reservations_.end() && node_owner->second != id)) continue;
            if (state.wait_started) state.accumulated_wait += now_ - *state.wait_started;
            state.wait_started.reset();
            state.pushback_wait_reported = false;
            state.phase = Phase::Pushback;
            state.pushback_started_at = now_;
            if (state.arrival_gate_at) {
                state.gate_occupancy_duration = now_ - *state.arrival_gate_at;
                state.gate_released_at = now_;
            }
            state.phase_started = now_;
            state.phase_end = now_ + config.pushback_duration;
            surface_edge_reservations_[selected_edge] = id;
            surface_node_reservations_[selected_node] = id;
            auto& assigned_gate = gate(aircraft(id).gate());
            if (assigned_gate.occupying_aircraft == id) assigned_gate.occupying_aircraft.reset();
            gate_arrival_reservations_.erase(aircraft(id).gate());
            emit(aircraft_event(SimulationEventType::SurfacePushbackStarted, aircraft(id)));
            emit(aircraft_event(SimulationEventType::PushbackTaxiOutStarted, aircraft(id)));
            event_for(SimulationEventType::SurfaceReservationAcquired, id, selected_edge);
            continue;
        }
        if (state.phase != Phase::Taxiing && state.phase != Phase::WaitingForTraffic) continue;
        if (state.phase_end) continue; // Aircraft committed to this edge; a closure takes effect at the next node.
        if (state.edge_index < state.route_edges.size()) {
            bool valid = true;
            for (std::size_t i = state.edge_index; i < state.route_edges.size(); ++i)
                valid = valid && scenario_.graph.edge(state.route_edges[i]).available;
            if (!valid) {
                std::optional<EdgeId> closed_edge;
                for (std::size_t i = state.edge_index; i < state.route_edges.size(); ++i) {
                    if (!scenario_.graph.edge(state.route_edges[i]).available) {
                        closed_edge = state.route_edges[i];
                        break;
                    }
                }
                event_for(SimulationEventType::SurfaceRouteInvalidated, id, closed_edge);
                const auto destination = state.arrival_operation ? aircraft(id).gate_node() : config.departure_handoff;
                const auto objective = state.arrival_operation ? agents::ObjectiveType::ReachAssignedStand : agents::ObjectiveType::DepartAirport;
                const auto decision = decide_surface_agent(id, destination, objective, true, closed_edge,
                    false, state.operator_triggered_invalidation);
                state.operator_triggered_invalidation = false;
                apply_surface_agent_route(id, destination, decision);
                if (decision.action != agents::ActionType::PlanRoute || !decision.route) {
                    event_for(SimulationEventType::SurfaceSafeFailure, id);
                    continue;
                }
                ++state.reroutes;
                auto rerouted = aircraft_event(SimulationEventType::SurfaceRerouted, aircraft(id));
                rerouted.route = *decision.route;
                emit(std::move(rerouted));
                auto route_event = aircraft_event(SimulationEventType::SurfaceTaxiRouteAssigned, aircraft(id));
                route_event.route = *decision.route;
                emit(std::move(route_event));
            }
        }
        if (state.edge_index >= state.route_edges.size()) continue;
        const auto edge_id = state.route_edges[state.edge_index];
        const auto& edge = scenario_.graph.edge(edge_id);
        const auto next_node = scenario_.graph.other_endpoint(edge, state.node);
        const auto occupied = surface_node_reservations_.find(next_node);
        const auto edge_owner = surface_edge_reservations_.find(edge_id);
        const auto ground_blocker = ground_blockers.find(edge_id);
        const bool blocked_by_stationary_ground_vehicle = ground_blocker != ground_blockers.end();
        if (!edge.available || blocked_by_stationary_ground_vehicle ||
            (edge.crosses_runway && runway_crossing_available_at_ > now_) ||
            (occupied != surface_node_reservations_.end() && occupied->second != id) ||
            (edge_owner != surface_edge_reservations_.end() && edge_owner->second != id)) {
            if (state.phase != Phase::WaitingForTraffic) {
                state.phase = Phase::WaitingForTraffic;
                state.wait_started = now_;
                ++surface_wait_events_;
                auto waiting = aircraft_event(SimulationEventType::SurfaceWaitingForTraffic, aircraft(id));
                waiting.edge = edge_id;
                if (blocked_by_stationary_ground_vehicle) {
                    waiting.vehicle = ground_blocker->second;
                    waiting.detail = "idle ground vehicle occupies the aircraft clearance area of the next edge";
                }
                emit(std::move(waiting));
            }
            const auto agent = surface_agents_.find(id);
            if ((blocked_by_stationary_ground_vehicle ||
                 config.aircraft_routing_policy != agents::AutonomyMode::Scripted) && state.wait_started &&
                (blocked_by_stationary_ground_vehicle || agent == surface_agents_.end() || !agent->second.memory.last_blockage_decision_at ||
                 now_ - *agent->second.memory.last_blockage_decision_at >= SimTime{30}) &&
                (blocked_by_stationary_ground_vehicle || now_ - *state.wait_started >= SimTime{30})) {
                surface_agents_.at(id).memory.last_blockage_decision_at = now_;
                const auto destination = state.arrival_operation ? aircraft(id).gate_node() : config.departure_handoff;
                const auto objective = state.arrival_operation ? agents::ObjectiveType::ReachAssignedStand : agents::ObjectiveType::DepartAirport;
                const auto decision = decide_surface_agent(id, destination, objective,
                    blocked_by_stationary_ground_vehicle, std::nullopt, !blocked_by_stationary_ground_vehicle);
                if (decision.action == agents::ActionType::PlanRoute && decision.route) {
                    apply_surface_agent_route(id, destination, decision);
                    ++state.reroutes;
                    auto rerouted = aircraft_event(SimulationEventType::SurfaceRerouted, aircraft(id));
                    rerouted.route = *decision.route;
                    emit(std::move(rerouted));
                    auto route_event = aircraft_event(SimulationEventType::SurfaceTaxiRouteAssigned, aircraft(id));
                    route_event.route = *decision.route;
                    emit(std::move(route_event));
                    continue;
                }
                if (decision.action == agents::ActionType::FailObjective) {
                    apply_surface_agent_route(id, destination, decision);
                    auto failure = aircraft_event(SimulationEventType::SurfaceSafeFailure, aircraft(id));
                    failure.edge = edge_id;
                    if (blocked_by_stationary_ground_vehicle) failure.vehicle = ground_blocker->second;
                    failure.detail = decision.explanation;
                    emit(std::move(failure));
                    continue;
                }
            }
            continue;
        }
        if (state.wait_started) state.accumulated_wait += now_ - *state.wait_started;
        state.phase = Phase::Taxiing;
        state.wait_started.reset();
        surface_node_reservations_.erase(state.node);
        surface_node_reservations_[next_node] = id;
        surface_edge_reservations_[edge_id] = id;
        event_for(SimulationEventType::SurfaceReservationAcquired, id, edge_id);
        state.phase_started = now_;
        const auto travel_seconds = static_cast<std::int64_t>(std::ceil(edge.distance_m / config.aircraft_speed_mps));
        state.phase_end = now_ + std::max(SimTime{1}, SimTime{travel_seconds});
        if (edge.crosses_runway) runway_crossing_available_at_ = *state.phase_end;
        state.taxi_distance_m += edge.distance_m;
        if (state.arrival_operation) state.arrival_taxi_distance_m += edge.distance_m;
        else state.departure_taxi_distance_m += edge.distance_m;
        if (!state.taxi_started_at) state.taxi_started_at = now_;
    }

    std::size_t taxiing_now = 0;
    for (const auto& [id, state] : surface_aircraft_) {
        (void)id;
        if (state.phase == Phase::Taxiing) ++taxiing_now;
    }
    maximum_simultaneous_taxiing_ = std::max(maximum_simultaneous_taxiing_, taxiing_now);
    const auto observed = snapshot();
    for (std::size_t first = 0; first < observed.aircraft.size(); ++first) {
        const auto& a = observed.aircraft[first].surface_position_m;
        if (!a) continue;
        for (std::size_t second = first + 1; second < observed.aircraft.size(); ++second) {
            const auto& b = observed.aircraft[second].surface_position_m;
            if (b) minimum_aircraft_separation_m_ = std::min(minimum_aircraft_separation_m_,
                std::hypot(a->x_m - b->x_m, a->y_m - b->y_m));
        }
    }
    // The aircraft/vehicle clearance margin exceeds one second of relative travel.
    std::set<AircraftId> clearance_waiting_now;
    const auto hold_for_clearance = [this, &clearance_waiting_now](AircraftId id, std::string detail,
        std::optional<VehicleId> other_vehicle = std::nullopt) {
        auto found = surface_aircraft_.find(id);
        if (found == surface_aircraft_.end() || found->second.phase == Phase::Failed || found->second.phase == Phase::Departed)
            return;
        auto& state = found->second;
        if (state.phase != Phase::Taxiing && state.phase != Phase::Pushback) return;
        clearance_waiting_now.insert(id);
        if (state.phase_started && state.phase_end) {
            state.phase_started = *state.phase_started + SimTime{1};
            state.phase_end = *state.phase_end + SimTime{1};
        }
        if (!state.clearance_waiting) {
            state.clearance_waiting = true;
            state.wait_started = now_;
            ++surface_wait_events_;
            auto event = aircraft_event(SimulationEventType::SurfaceWaitingForTraffic, aircraft(id));
            event.vehicle = other_vehicle;
            if (state.edge_index < state.route_edges.size()) event.edge = state.route_edges[state.edge_index];
            event.detail = std::move(detail);
            emit(std::move(event));
        }
    };
    for (std::size_t first = 0; first < observed.aircraft.size(); ++first) {
        const auto& a = observed.aircraft[first];
        if (!a.surface_position_m || !surface_aircraft_.contains(a.id)) continue;
        const auto phase = surface_aircraft_.at(a.id).phase;
        // Aircraft waiting for turnaround completion are parked at their gates. Ground
        // service vehicles are expected to be nearby during servicing, so collision
        // interlocks begin when the aircraft is authorized to occupy surface lanes.
        if (phase != Phase::ArrivalQueue && phase != Phase::Pushback && phase != Phase::Taxiing &&
            phase != Phase::WaitingForTraffic && phase != Phase::WaitingForRunway &&
            phase != Phase::Runway) continue;
        if (phase == Phase::Failed || phase == Phase::Departed || phase == Phase::None) continue;
        for (std::size_t second = first + 1; second < observed.aircraft.size(); ++second) {
            const auto& b = observed.aircraft[second];
            if (!b.surface_position_m) continue;
            const double distance = std::hypot(a.surface_position_m->x_m - b.surface_position_m->x_m,
                a.surface_position_m->y_m - b.surface_position_m->y_m);
            const auto separation = surface_safety::aircraft_separation(distance);
            if (separation.hold) {
                const auto a_phase = surface_aircraft_.at(a.id).phase;
                const auto b_phase = surface_aircraft_.contains(b.id) ? surface_aircraft_.at(b.id).phase : Phase::None;
                const bool a_moving = a_phase == Phase::Taxiing || a_phase == Phase::Pushback;
                const bool b_moving = b_phase == Phase::Taxiing || b_phase == Phase::Pushback;
                if (a_moving && b_moving)
                    hold_for_clearance(std::max(a.id, b.id), "aircraft-aircraft 20 m clearance");
                else if (a_moving) hold_for_clearance(a.id, "aircraft-aircraft 20 m clearance");
                else if (b_moving) hold_for_clearance(b.id, "aircraft-aircraft 20 m clearance");
                if (separation.collision) ++surface_aircraft_aircraft_collisions_;
            }
        }
        double nearest_vehicle_distance = std::numeric_limits<double>::infinity();
        std::optional<VehicleId> nearest_vehicle;
        for (const auto& ground_vehicle : observed.vehicles) {
            if (!ground_vehicle.observed_position_m) continue;
            const double distance = std::hypot(a.surface_position_m->x_m - ground_vehicle.observed_position_m->x_m,
                a.surface_position_m->y_m - ground_vehicle.observed_position_m->y_m);
            minimum_aircraft_ground_separation_m_ = std::min(minimum_aircraft_ground_separation_m_, distance);
            if (distance < nearest_vehicle_distance) {
                nearest_vehicle_distance = distance;
                nearest_vehicle = ground_vehicle.id;
            }
        }
        const auto ground_separation = surface_safety::aircraft_ground_separation(nearest_vehicle_distance);
        if (ground_separation.hold) {
            hold_for_clearance(a.id, "aircraft-ground vehicle 30 m sampled clearance", nearest_vehicle);
            if (ground_separation.collision) ++surface_aircraft_ground_collisions_;
        }
    }
    for (auto& [id, state] : surface_aircraft_) {
        if (!state.clearance_waiting || clearance_waiting_now.contains(id)) continue;
        if (state.wait_started) state.accumulated_wait += now_ - *state.wait_started;
        state.wait_started.reset();
        state.clearance_waiting = false;
    }

    // Shared runway policy: FIFO simulated request timestamp, then stable aircraft ID.
    // This work-conserving aging policy prevents starvation for finite arrivals/departures.
    if (runway_available_at_ <= now_ && runway_crossing_available_at_ <= now_) {
        std::vector<AircraftId> queued;
        for (const auto& [id, state] : surface_aircraft_)
            if ((state.phase == Phase::WaitingForRunway || state.phase == Phase::ArrivalQueue) &&
                (state.arrival_operation || aircraft(id).scheduled_departure() <= now_))
                queued.push_back(id);
        std::ranges::sort(queued, [this](AircraftId left, AircraftId right) {
            const auto l = surface_aircraft_.at(left).queued_at;
            const auto r = surface_aircraft_.at(right).queued_at;
            return l == r ? left < right : l < r;
        });
        if (!queued.empty()) {
            const auto id = queued.front();
            auto& state = surface_aircraft_.at(id);
            if (state.wait_started) state.accumulated_wait += now_ - *state.wait_started;
            state.wait_started.reset();
            state.runway_wait = now_ - state.queued_at;
            if (state.arrival_operation) state.arrival_runway_wait = state.runway_wait;
            else state.departure_runway_wait = state.runway_wait;
            state.phase = Phase::Runway;
            state.phase_started = now_;
            state.runway_clearance_at = now_;
            if (state.arrival_operation) state.arrival_runway_clearance_time = now_;
            else state.departure_runway_clearance_time = now_;
            state.phase_end = now_ + (state.arrival_operation ? config.arrival_rollout : config.runway_occupancy);
            if (state.arrival_operation) {
                aircraft(id).mark_landed(now_);
                event_for(SimulationEventType::AircraftLanded, id);
            }
            runway_available_at_ = *state.phase_end;
            event_for(SimulationEventType::SurfaceRunwayClearance, id);
            event_for(SimulationEventType::RunwayGrant, id);
            event_for(SimulationEventType::RunwayOccupied, id);
        }
    }
    maximum_runway_queue_depth_ = std::max(maximum_runway_queue_depth_, static_cast<std::size_t>(std::ranges::count_if(
        surface_aircraft_, [](const auto& item) {
            return item.second.phase == Phase::WaitingForRunway || item.second.phase == Phase::ArrivalQueue;
        })));

    const bool active = std::ranges::any_of(surface_aircraft_, [](const auto& item) {
        const auto phase = item.second.phase;
        return phase != Phase::None && phase != Phase::Departed && phase != Phase::Arrived && phase != Phase::Failed;
    });
    if (active && !surface_tick_scheduled_) {
        surface_tick_scheduled_ = true;
        auto next_tick = now_ + SimTime{1};
        const bool stationary_departure_queue = std::ranges::all_of(surface_aircraft_, [](const auto& item) {
            const auto phase = item.second.phase;
            return phase == Phase::None || phase == Phase::Departed || phase == Phase::Arrived || phase == Phase::Failed ||
                (phase == Phase::WaitingForRunway && !item.second.arrival_operation);
        });
        if (stationary_departure_queue) {
            std::optional<SimTime> next_departure;
            for (const auto& [id, state] : surface_aircraft_) {
                if (state.phase != Phase::WaitingForRunway) continue;
                const auto eligible = std::max({aircraft(id).scheduled_departure(), runway_available_at_, runway_crossing_available_at_});
                if (eligible > now_ && (!next_departure || eligible < *next_departure)) next_departure = eligible;
            }
            if (const auto queued_event = next_event_time(); queued_event && *queued_event > now_ &&
                (!next_departure || *queued_event < *next_departure)) next_departure = queued_event;
            if (next_departure) next_tick = *next_departure;
        }
        [[maybe_unused]] const auto sequence = events_.schedule(next_tick, EventType::SurfaceTick);
    }
}

void Simulation::emit(SimulationEventRecord event) {
    event.sequence = next_event_record_sequence_++;
    event.timestamp = now_;
    if (event.aircraft || event.vehicle) {
        const auto state = snapshot();
        if (event.aircraft) {
            const auto item = std::ranges::find(state.aircraft, *event.aircraft, &AircraftSnapshot::id);
            if (item != state.aircraft.end()) {
                if (item->surface_position_m) {
                    event.position_m = item->surface_position_m;
                    event.heading_rad = item->surface_heading_rad;
                }
                event.aircraft_state = item->state;
            }
            if (event.vehicle) {
                const auto related = std::ranges::find(state.vehicles, *event.vehicle, &ServiceVehicleSnapshot::id);
                if (related != state.vehicles.end()) {
                    event.vehicle_state = related->state;
                    event.related_position_m = related->observed_position_m;
                    event.related_heading_rad = related->observed_heading_rad;
                }
            }
        } else if (event.vehicle) {
            const auto item = std::ranges::find(state.vehicles, *event.vehicle, &ServiceVehicleSnapshot::id);
            if (item != state.vehicles.end()) {
                if (item->observed_position_m) {
                    event.position_m = item->observed_position_m;
                    event.heading_rad = item->observed_heading_rad;
                }
                event.vehicle_state = item->state;
            }
        }
    }
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
    auto& resource_pool = type == ServiceType::Fueling ? fuel_pool_ : baggage_pool_;
    if (!resource_pool) throw std::logic_error("scenario has no mobile resource pool for required service");
    return *resource_pool;
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
