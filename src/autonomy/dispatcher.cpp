#include "airside/autonomy/dispatcher.hpp"

#include "airside/routing/astar.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <tuple>

namespace airside::autonomy {
namespace {
bool terminal(ServiceTaskState state) {
    return state == ServiceTaskState::Completed || state == ServiceTaskState::Failed;
}

std::optional<NodeId> node_named(const AirportGraph& graph, const std::string& name) {
    for (const auto& node : graph.nodes()) if (node.name == name) return node.id;
    return std::nullopt;
}

std::int64_t effective_priority(const ServiceRequest& request, double released_at_s,
                                double now_s, double interval_s) {
    const auto age_levels = static_cast<std::int64_t>(std::floor(std::max(0.0, now_s - released_at_s) / interval_s));
    if (request.priority > 0 && age_levels > std::numeric_limits<std::int64_t>::max() - request.priority)
        return std::numeric_limits<std::int64_t>::max();
    if (request.priority < 0 && age_levels < std::numeric_limits<std::int64_t>::min() - request.priority)
        return std::numeric_limits<std::int64_t>::max();
    return static_cast<std::int64_t>(request.priority) + age_levels;
}
}

FleetDispatcher::FleetDispatcher(std::vector<ServiceRequest> requests, double aging_interval_s)
    : aging_interval_s_(aging_interval_s) {
    if (!std::isfinite(aging_interval_s_) || aging_interval_s_ <= 0.0)
        throw std::invalid_argument("dispatch aging interval must be finite and positive");
    std::ranges::sort(requests, {}, &ServiceRequest::id);
    for (std::size_t i = 0; i < requests.size(); ++i) {
        const auto& request = requests[i];
        if (request.id.value.empty() || (i != 0 && requests[i - 1].id == request.id))
            throw std::invalid_argument("service request IDs must be nonempty and unique");
        if (request.required_capability.empty() || request.origin.empty() || request.destination.empty() ||
            !std::isfinite(request.release_time_s) || request.release_time_s < 0.0 ||
            !std::isfinite(request.service_duration_s) || request.service_duration_s < 0.0 ||
            (request.deadline_s && (!std::isfinite(*request.deadline_s) || *request.deadline_s < request.release_time_s)))
            throw std::invalid_argument("service request has invalid capability, route, release, deadline, or duration");
        tasks_.push_back({request});
    }
}

FleetDispatcher::TaskRecord& FleetDispatcher::find(const ServiceRequestId& request) {
    const auto it = std::ranges::find(tasks_, request, [](const auto& task) { return task.request.id; });
    if (it == tasks_.end()) throw std::out_of_range("unknown service request: " + request.value);
    return *it;
}
const FleetDispatcher::TaskRecord& FleetDispatcher::find(const ServiceRequestId& request) const {
    const auto it = std::ranges::find(tasks_, request, [](const auto& task) { return task.request.id; });
    if (it == tasks_.end()) throw std::out_of_range("unknown service request: " + request.value);
    return *it;
}

void FleetDispatcher::emit(double time_s, DispatchEventKind kind, const TaskRecord& task,
                           VehicleId vehicle, std::string detail, double route_distance_m) {
    events_.push_back({time_s, kind, task.request.id, std::move(vehicle), std::move(detail),
                       effective_priority(task.request, task.released_at_s, time_s, aging_interval_s_),
                       route_distance_m});
}

void FleetDispatcher::release_due(double simulation_time_s) {
    if (!std::isfinite(simulation_time_s) || simulation_time_s < last_time_s_)
        throw std::invalid_argument("dispatcher time must be finite and monotonic");
    last_time_s_ = simulation_time_s;
    for (auto& task : tasks_) {
        if (task.state != ServiceTaskState::Queued || task.released || task.request.release_time_s > simulation_time_s)
            continue;
        task.released = true;
        task.released_at_s = task.request.release_time_s;
        task.queued_since_s = simulation_time_s;
        emit(task.request.release_time_s, DispatchEventKind::RequestReleased, task, {}, task.request.origin + "->" + task.request.destination);
    }
}

std::vector<std::pair<ServiceRequestId, VehicleId>> FleetDispatcher::dispatch(
    double simulation_time_s, const std::vector<DispatchVehicle>& vehicles, const AirportGraph& graph) {
    release_due(simulation_time_s);
    std::vector<TaskRecord*> queued;
    for (auto& task : tasks_) {
        if (task.state != ServiceTaskState::Queued || !task.released ||
            task.request.release_time_s > simulation_time_s) continue;
        const auto priority = effective_priority(task.request, task.released_at_s, simulation_time_s, aging_interval_s_);
        const auto age_level = priority - static_cast<std::int64_t>(task.request.priority);
        while (task.last_aging_level < age_level) {
            ++task.last_aging_level;
            ++aging_activations_;
            emit(simulation_time_s, DispatchEventKind::AgingApplied, task, {}, "waiting_priority_increment");
        }
        queued.push_back(&task);
    }
    std::ranges::sort(queued, [&](const TaskRecord* a, const TaskRecord* b) {
        const auto pa = effective_priority(a->request, a->released_at_s, simulation_time_s, aging_interval_s_);
        const auto pb = effective_priority(b->request, b->released_at_s, simulation_time_s, aging_interval_s_);
        if (pa != pb) return pa > pb;
        if (a->released_at_s != b->released_at_s) return a->released_at_s < b->released_at_s;
        return a->request.id < b->request.id;
    });

    std::vector<std::pair<ServiceRequestId, VehicleId>> assigned;
    std::vector<VehicleId> occupied;
    for (auto* task : queued) {
        const auto origin = node_named(graph, task->request.origin);
        const auto destination = node_named(graph, task->request.destination);
        if (!origin || !destination) {
            emit(simulation_time_s, DispatchEventKind::CandidateEvaluated, *task, {}, "unknown_origin");
            continue;
        }
        if (!airside::find_route(graph, *origin, *destination)) {
            emit(simulation_time_s, DispatchEventKind::CandidateEvaluated, *task, {}, "no_available_task_route");
            continue;
        }
        struct Candidate { const DispatchVehicle* vehicle{}; double distance{}; };
        std::optional<Candidate> best;
        for (const auto& vehicle : vehicles) {
            if (vehicle.id.value.empty() || vehicle.state != DispatchVehicleState::Idle ||
                std::ranges::find(occupied, vehicle.id) != occupied.end() || vehicle.current_request ||
                active_assignments_.contains(vehicle.id)) continue;
            if (std::ranges::find(vehicle.capabilities, task->request.required_capability) == vehicle.capabilities.end()) {
                emit(simulation_time_s, DispatchEventKind::CandidateEvaluated, *task, vehicle.id,
                     "incompatible:" + task->request.required_capability);
                continue;
            }
            const auto current = node_named(graph, vehicle.current_node);
            if (!current) {
                emit(simulation_time_s, DispatchEventKind::CandidateEvaluated, *task, vehicle.id, "unknown_vehicle_position");
                continue;
            }
            const auto route = airside::find_route(graph, *current, *origin);
            if (!route) {
                emit(simulation_time_s, DispatchEventKind::CandidateEvaluated, *task, vehicle.id, "no_available_route");
                continue;
            }
            emit(simulation_time_s, DispatchEventKind::CandidateEvaluated, *task, vehicle.id,
                 "compatible", route->distance_m);
            if (!best || route->distance_m < best->distance ||
                (route->distance_m == best->distance && vehicle.id < best->vehicle->id))
                best = Candidate{&vehicle, route->distance_m};
        }
        if (!best) continue;
        const bool reassigned = task->reassignments != 0;
        task->state = ServiceTaskState::Assigned;
        task->assigned_vehicle = best->vehicle->id;
        task->queue_wait_s += std::max(0.0, simulation_time_s - task->queued_since_s);
        task->assigned_at_s = simulation_time_s;
        active_assignments_.emplace(best->vehicle->id, task->request.id);
        occupied.push_back(best->vehicle->id);
        ++assignments_;
        emit(simulation_time_s, reassigned ? DispatchEventKind::Reassigned : DispatchEventKind::Assigned,
             *task, best->vehicle->id, "minimum_available_route_cost;stable_vehicle_id_tie_break", best->distance);
        assigned.emplace_back(task->request.id, best->vehicle->id);
    }
    return assigned;
}

void FleetDispatcher::set_state(const ServiceRequestId& request, ServiceTaskState state, double simulation_time_s) {
    if (!std::isfinite(simulation_time_s) || simulation_time_s < last_time_s_)
        throw std::invalid_argument("dispatcher time must be finite and monotonic");
    auto& task = find(request);
    if (!task.released) throw std::invalid_argument("unreleased service request cannot change lifecycle state");
    const bool allowed = (task.state == ServiceTaskState::Assigned && state == ServiceTaskState::EnRoute) ||
                         (task.state == ServiceTaskState::EnRoute && state == ServiceTaskState::Servicing) ||
                         (task.state == ServiceTaskState::Servicing && state == ServiceTaskState::Completed) ||
                         (state == ServiceTaskState::Failed && !terminal(task.state));
    if (!allowed) throw std::invalid_argument("invalid service request lifecycle transition");
    task.state = state;
    const auto assigned_vehicle = task.assigned_vehicle.value_or(VehicleId{});
    if (state == ServiceTaskState::Completed) {
        task.completed_at_s = simulation_time_s;
        if (task.assigned_vehicle) active_assignments_.erase(*task.assigned_vehicle);
    } else if (state == ServiceTaskState::Failed && task.assigned_vehicle) {
        active_assignments_.erase(*task.assigned_vehicle);
    }
    emit(simulation_time_s, state == ServiceTaskState::Completed ? DispatchEventKind::Completed :
         state == ServiceTaskState::Failed ? DispatchEventKind::Failed : DispatchEventKind::StateChanged,
          task, assigned_vehicle, to_string(state));
    last_time_s_ = simulation_time_s;
}

void FleetDispatcher::mark_vehicle_unavailable(const VehicleId& vehicle, double simulation_time_s) {
    if (!std::isfinite(simulation_time_s) || simulation_time_s < last_time_s_)
        throw std::invalid_argument("dispatcher time must be finite and monotonic");
    for (auto& task : tasks_) {
        if (!task.assigned_vehicle || *task.assigned_vehicle != vehicle || terminal(task.state)) continue;
        emit(simulation_time_s, DispatchEventKind::Unassigned, task, vehicle, "vehicle_unavailable");
        active_assignments_.erase(vehicle);
        task.assigned_vehicle.reset();
        task.state = ServiceTaskState::Queued;
        task.queued_since_s = simulation_time_s;
        ++task.reassignments;
        ++reassignments_;
        emit(simulation_time_s, DispatchEventKind::Requeued, task, vehicle, "safe_requeue_without_progress_transfer");
    }
    last_time_s_ = simulation_time_s;
}

void FleetDispatcher::fail(const ServiceRequestId& request, double simulation_time_s, std::string reason) {
    if (!std::isfinite(simulation_time_s) || simulation_time_s < last_time_s_)
        throw std::invalid_argument("dispatcher time must be finite and monotonic");
    auto& task = find(request);
    if (!task.released) throw std::invalid_argument("unreleased service request cannot fail");
    if (terminal(task.state)) throw std::invalid_argument("terminal service request cannot be failed again");
    task.state = ServiceTaskState::Failed;
    if (task.assigned_vehicle) active_assignments_.erase(*task.assigned_vehicle);
    emit(simulation_time_s, DispatchEventKind::Failed, task, task.assigned_vehicle.value_or(VehicleId{}), std::move(reason));
    last_time_s_ = simulation_time_s;
}

void FleetDispatcher::fail_unfinished(double simulation_time_s, std::string reason) {
    if (!std::isfinite(simulation_time_s) || simulation_time_s < last_time_s_)
        throw std::invalid_argument("dispatcher time must be finite and monotonic");
    release_due(simulation_time_s);
    for (auto& task : tasks_) {
        if (terminal(task.state)) continue;
        if (task.assigned_vehicle) active_assignments_.erase(*task.assigned_vehicle);
        task.state = ServiceTaskState::Failed;
        emit(simulation_time_s, DispatchEventKind::Failed, task,
             task.assigned_vehicle.value_or(VehicleId{}), reason);
    }
    last_time_s_ = simulation_time_s;
}

std::vector<ServiceTaskSnapshot> FleetDispatcher::snapshot() const {
    std::vector<ServiceTaskSnapshot> result;
    result.reserve(tasks_.size());
    for (const auto& task : tasks_) {
        const double now = last_time_s_;
        result.push_back({task.request, task.state, task.assigned_vehicle, task.released_at_s,
                          task.assigned_at_s, task.completed_at_s, task.queue_wait_s, task.reassignments,
                          !task.released ? static_cast<std::int64_t>(task.request.priority) :
                          effective_priority(task.request, task.released_at_s, now, aging_interval_s_)});
    }
    return result;
}

bool FleetDispatcher::all_terminal() const noexcept {
    return std::ranges::all_of(tasks_, [](const auto& task) { return terminal(task.state); });
}

std::optional<double> FleetDispatcher::next_release_time_s() const noexcept {
    std::optional<double> next;
    for (const auto& task : tasks_) {
        if (task.released || terminal(task.state)) continue;
        if (!next || task.request.release_time_s < *next) next = task.request.release_time_s;
    }
    return next;
}

FleetDispatchMetrics FleetDispatcher::metrics() const {
    FleetDispatchMetrics result;
    result.requests_created = tasks_.size();
    result.assignments = assignments_;
    result.reassignments = reassignments_;
    result.aging_activations = aging_activations_;
    result.events = events_;
    result.requests = snapshot();
    double wait_total = 0.0;
    for (const auto& task : tasks_) {
        if (task.state == ServiceTaskState::Completed) {
            ++result.requests_completed;
            const double wait = task.queue_wait_s;
            wait_total += wait;
            result.maximum_queue_wait_s = std::max(result.maximum_queue_wait_s, wait);
            result.maximum_completion_time_s = std::max(result.maximum_completion_time_s, task.completed_at_s - task.released_at_s);
            if (task.request.deadline_s && task.completed_at_s > *task.request.deadline_s) ++result.deadline_misses;
        } else if (task.state == ServiceTaskState::Failed) ++result.requests_failed;
        else ++result.unfinished_requests;
    }
    result.total_queue_wait_s = wait_total;
    result.average_queue_wait_s = result.requests_completed == 0 ? 0.0 : wait_total / static_cast<double>(result.requests_completed);
    return result;
}

std::string to_string(ServiceKind kind) {
    switch (kind) {
    case ServiceKind::BaggageDelivery: return "baggage_delivery";
    case ServiceKind::FuelService: return "fuel_service";
    case ServiceKind::AircraftTurnaround: return "aircraft_turnaround";
    case ServiceKind::TugCartMovement: return "tug_cart_movement";
    }
    return "unknown";
}
std::string to_string(ServiceTaskState state) {
    switch (state) {
    case ServiceTaskState::Queued: return "queued";
    case ServiceTaskState::Assigned: return "assigned";
    case ServiceTaskState::EnRoute: return "en_route";
    case ServiceTaskState::Servicing: return "servicing";
    case ServiceTaskState::Completed: return "completed";
    case ServiceTaskState::Failed: return "failed_or_timed_out";
    }
    return "unknown";
}
std::string to_string(DispatchVehicleState state) {
    switch (state) {
    case DispatchVehicleState::Idle: return "idle";
    case DispatchVehicleState::Busy: return "busy";
    case DispatchVehicleState::Degraded: return "degraded";
    case DispatchVehicleState::Unavailable: return "unavailable";
    case DispatchVehicleState::Recovering: return "recovering";
    }
    return "unknown";
}
std::string to_string(DispatchEventKind kind) {
    switch (kind) {
    case DispatchEventKind::RequestReleased: return "service_request_released";
    case DispatchEventKind::CandidateEvaluated: return "dispatch_candidate_evaluated";
    case DispatchEventKind::Assigned: return "task_assigned";
    case DispatchEventKind::StateChanged: return "task_state_changed";
    case DispatchEventKind::Unassigned: return "task_unassigned";
    case DispatchEventKind::Requeued: return "task_requeued";
    case DispatchEventKind::Reassigned: return "task_reassigned";
    case DispatchEventKind::AgingApplied: return "dispatch_aging_applied";
    case DispatchEventKind::Completed: return "task_completed";
    case DispatchEventKind::Failed: return "task_failed";
    }
    return "unknown";
}

} // namespace airside::autonomy
