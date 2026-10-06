#include "airside/agents/aircraft.hpp"

#include <algorithm>
#include <format>
#include <stdexcept>
#include <utility>

namespace airside {

Aircraft::Aircraft(
    AircraftId id,
    std::string flight_number,
    SimTime scheduled_arrival,
    SimTime scheduled_departure,
    GateId gate,
    NodeId gate_node,
    std::vector<ServiceTask> tasks,
    std::string turnaround_id,
    std::optional<SimTime> target_off_block,
    AircraftOperationType operation_type,
    NodeId arrival_exit_node)
    : id_(id),
      flight_number_(std::move(flight_number)),
      scheduled_arrival_(scheduled_arrival),
      scheduled_departure_(scheduled_departure),
      gate_(gate),
      gate_node_(gate_node),
      tasks_(std::move(tasks)),
      turnaround_id_(turnaround_id.empty() ? flight_number_ : std::move(turnaround_id)),
      target_off_block_(target_off_block.value_or(scheduled_departure)),
      operation_type_(operation_type), arrival_exit_node_(arrival_exit_node) {
    if (flight_number_.empty() || scheduled_departure_ < scheduled_arrival_ ||
        (tasks_.empty() && operation_type_ != AircraftOperationType::ArrivalOnly)) {
        throw std::invalid_argument(std::format("invalid aircraft schedule or required tasks for '{}' (arrival={}, departure={}, tasks={}, operation={})",
            flight_number_, scheduled_arrival_.count(), scheduled_departure_.count(), tasks_.size(),
            operation_type_ == AircraftOperationType::ArrivalOnly ? "arrival" : "turnaround"));
    }
}

AircraftId Aircraft::id() const noexcept { return id_; }
const std::string& Aircraft::flight_number() const noexcept { return flight_number_; }
SimTime Aircraft::scheduled_arrival() const noexcept { return scheduled_arrival_; }
SimTime Aircraft::scheduled_departure() const noexcept { return scheduled_departure_; }
GateId Aircraft::gate() const noexcept { return gate_; }
NodeId Aircraft::gate_node() const noexcept { return gate_node_; }
AircraftState Aircraft::state() const noexcept { return state_; }
std::optional<SimTime> Aircraft::actual_arrival() const noexcept { return actual_arrival_; }
std::optional<SimTime> Aircraft::ready_at() const noexcept { return ready_at_; }
std::optional<SimTime> Aircraft::actual_departure() const noexcept { return actual_departure_; }
const std::vector<ServiceTask>& Aircraft::tasks() const noexcept { return tasks_; }
std::vector<ServiceTask>& Aircraft::mutable_tasks() noexcept { return tasks_; }
const std::string& Aircraft::turnaround_id() const noexcept { return turnaround_id_; }
SimTime Aircraft::target_off_block() const noexcept { return target_off_block_; }
AircraftOperationType Aircraft::operation_type() const noexcept { return operation_type_; }
NodeId Aircraft::arrival_exit_node() const noexcept { return arrival_exit_node_; }
TurnaroundState Aircraft::turnaround_state() const noexcept {
    if (failed_) return TurnaroundState::Failed;
    switch (state_) {
    case AircraftState::Scheduled: return TurnaroundState::Scheduled;
    case AircraftState::ReadyForPushback: return TurnaroundState::ReadyForDeparture;
    case AircraftState::Departed: return TurnaroundState::Departed;
    default:
        return std::ranges::any_of(tasks_, [](const auto& value) {
            return value.status == TaskStatus::InProgress || value.status == TaskStatus::Assigned ||
                   value.status == TaskStatus::Waiting;
        }) ? TurnaroundState::Servicing : TurnaroundState::Arrived;
    }
}
std::optional<SimTime> Aircraft::departure_delay() const noexcept {
    if (!actual_departure_) return std::nullopt;
    return std::max(SimTime::zero(), *actual_departure_ - scheduled_departure_);
}
const std::string& Aircraft::failure_reason() const noexcept { return failure_reason_; }

bool Aircraft::can_transition(AircraftState from, AircraftState to) noexcept {
    switch (from) {
    case AircraftState::Scheduled: return to == AircraftState::Arriving;
    case AircraftState::Arriving: return to == AircraftState::AtGate;
    case AircraftState::AtGate: return to == AircraftState::WaitingForServices;
    case AircraftState::WaitingForServices: return to == AircraftState::ReadyForPushback;
    case AircraftState::ReadyForPushback: return to == AircraftState::Departed;
    case AircraftState::Departed: return false;
    }
    return false;
}

void Aircraft::transition_to(AircraftState next) {
    if (!can_transition(state_, next)) {
        throw std::logic_error("invalid aircraft state transition");
    }
    state_ = next;
}

void Aircraft::arrive(SimTime now) {
    if (now < scheduled_arrival_) {
        throw std::logic_error("aircraft cannot arrive before its scheduled event");
    }
    if (state_ == AircraftState::Scheduled) transition_to(AircraftState::Arriving);
    else if (state_ != AircraftState::Arriving) throw std::logic_error("aircraft cannot complete arrival in its current state");
    transition_to(AircraftState::AtGate);
    actual_arrival_ = now;
    if (operation_type_ == AircraftOperationType::Turnaround) transition_to(AircraftState::WaitingForServices);
}

void Aircraft::begin_surface_arrival() {
    if (operation_type_ != AircraftOperationType::ArrivalOnly || state_ != AircraftState::Scheduled)
        throw std::logic_error("only a scheduled inbound aircraft may enter the arrival operation");
    transition_to(AircraftState::Arriving);
}

void Aircraft::mark_task_waiting(ServiceType type, SimTime now) {
    mark_task_waiting(mutable_task(type).id, now);
}

void Aircraft::mark_task_waiting(TaskId id, SimTime now) {
    auto& value = mutable_task(id);
    if (value.status != TaskStatus::Pending) {
        throw std::logic_error("only a pending service task may wait");
    }
    value.status = TaskStatus::Waiting;
    value.requested_at = now;
}

void Aircraft::make_task_ready(TaskId id) {
    auto& value = mutable_task(id);
    if (value.status != TaskStatus::Blocked) return;
    value.status = TaskStatus::Pending;
}

void Aircraft::assign_task(ServiceType type) {
    assign_task(mutable_task(type).id);
}

void Aircraft::assign_task(TaskId id, std::string resource, std::optional<VehicleId> vehicle) {
    auto& value = mutable_task(id);
    if (value.status != TaskStatus::Pending && value.status != TaskStatus::Waiting) {
        throw std::logic_error("service task cannot be assigned in its current state");
    }
    value.status = TaskStatus::Assigned;
    value.assigned_resource = std::move(resource);
    value.assigned_vehicle = vehicle;
}

void Aircraft::requeue_task(TaskId id, SimTime now) {
    auto& value = mutable_task(id);
    if (value.status != TaskStatus::Assigned) throw std::logic_error("only a dispatched task may be reassigned");
    if (value.assigned_vehicle) ++value.reassignments;
    value.assigned_vehicle.reset();
    value.assigned_resource.clear();
    value.status = TaskStatus::Waiting;
    if (!value.requested_at) value.requested_at = now;
}

void Aircraft::start_task(ServiceType type, SimTime now) {
    start_task(mutable_task(type).id, now);
}

void Aircraft::start_task(TaskId id, SimTime now) {
    auto& value = mutable_task(id);
    if (value.status != TaskStatus::Assigned) {
        throw std::logic_error("service task must be assigned before it starts");
    }
    value.status = TaskStatus::InProgress;
    value.started_at = now;
    if (!value.requested_at.has_value()) {
        // Tasks blocked on prerequisites become requests only when they become
        // eligible; a task that can start immediately has no resource wait.
        value.requested_at = now;
    }
}

void Aircraft::complete_task(ServiceType type, SimTime now) {
    complete_task(mutable_task(type).id, now);
}

void Aircraft::complete_task(TaskId id, SimTime now) {
    auto& value = mutable_task(id);
    if (value.status != TaskStatus::InProgress || !value.started_at || now < *value.started_at) {
        throw std::logic_error(std::format("service task {} cannot complete before it starts (state={}, started={}, now={})",
            id.value(), static_cast<int>(value.status), value.started_at ? value.started_at->count() : -1, now.count()));
    }
    value.status = TaskStatus::Completed;
    value.completed_at = now;
    if (services_complete()) {
        transition_to(AircraftState::ReadyForPushback);
        ready_at_ = now;
    }
}

void Aircraft::set_task_duration(TaskId id, SimTime duration) {
    if (duration <= SimTime::zero()) throw std::invalid_argument("task duration must be positive");
    auto& value = mutable_task(id);
    if (value.status == TaskStatus::InProgress || value.status == TaskStatus::Completed) {
        throw std::logic_error("cannot change a task duration after service starts");
    }
    value.duration = duration;
}

void Aircraft::fail_task(TaskId id, std::string reason) {
    auto& value = mutable_task(id);
    if (value.status == TaskStatus::Completed || value.status == TaskStatus::Failed)
        throw std::logic_error("terminal service task cannot fail again");
    value.status = TaskStatus::Failed;
    failed_ = true;
    if (failure_reason_.empty()) failure_reason_ = std::move(reason);
}

void Aircraft::fail_turnaround(std::string reason) {
    failed_ = true;
    if (failure_reason_.empty()) failure_reason_ = std::move(reason);
    for (auto& value : tasks_) {
        if (value.status != TaskStatus::Completed) value.status = TaskStatus::Failed;
    }
}

void Aircraft::depart(SimTime now) {
    if (!services_complete() || state_ != AircraftState::ReadyForPushback) {
        throw std::logic_error("aircraft cannot depart before all required services finish");
    }
    if (ready_at_ && now < *ready_at_) {
        throw std::logic_error("aircraft departure precedes readiness");
    }
    transition_to(AircraftState::Departed);
    actual_departure_ = now;
}

bool Aircraft::services_complete() const noexcept {
    return std::ranges::all_of(tasks_, [](const auto& value) {
        return value.status == TaskStatus::Completed;
    });
}

const ServiceTask& Aircraft::task(ServiceType type) const {
    return const_cast<Aircraft*>(this)->mutable_task(type);
}

const ServiceTask& Aircraft::task(TaskId id) const {
    const auto found = std::ranges::find_if(tasks_, [id](const auto& value) { return value.id == id; });
    if (found == tasks_.end()) throw std::out_of_range("aircraft does not contain this task ID");
    return *found;
}

ServiceTask& Aircraft::mutable_task(TaskId id) {
    const auto found = std::ranges::find_if(tasks_, [id](const auto& value) { return value.id == id; });
    if (found == tasks_.end()) throw std::out_of_range("aircraft does not contain this task ID");
    return *found;
}

ServiceTask& Aircraft::mutable_task(ServiceType type) {
    const auto found = std::ranges::find_if(tasks_, [&](const auto& value) { return value.type == type; });
    if (found == tasks_.end()) {
        throw std::out_of_range("aircraft does not require this service type");
    }
    return *found;
}

}  // namespace airside
