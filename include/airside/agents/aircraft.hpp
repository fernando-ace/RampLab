#pragma once

#include "airside/core/types.hpp"

#include <optional>
#include <string>
#include <vector>

namespace airside {

enum class AircraftState {
    Scheduled,
    Arriving,
    AtGate,
    WaitingForServices,
    ReadyForPushback,
    Departed,
};

enum class ServiceType {
    Fueling, Baggage, Deboarding, Catering, CabinCleaning, BaggageLoad, PushbackPreparation
};
enum class TaskStatus { Blocked, Pending, Waiting, Assigned, InProgress, Completed, Failed };
enum class TurnaroundState { Scheduled, Arrived, Servicing, ReadyForDeparture, Departed, Delayed, Failed };

struct ServiceTask {
    TaskId id;
    ServiceType type;
    TaskStatus status{TaskStatus::Pending};
    std::optional<SimTime> requested_at;
    std::optional<SimTime> started_at;
    std::optional<SimTime> completed_at;
    std::vector<TaskId> prerequisites;
    SimTime earliest_start{};
    std::optional<SimTime> latest_desirable_completion;
    SimTime duration{};
    std::string required_resource;
    std::optional<VehicleId> assigned_vehicle;
    std::string assigned_resource;
    std::size_t reassignments{};
    bool eligibility_scheduled{false};

    constexpr ServiceTask(TaskId task_id, ServiceType service_type) noexcept
        : id(task_id), type(service_type) {}
};

class Aircraft {
public:
    Aircraft(
        AircraftId id,
        std::string flight_number,
        SimTime scheduled_arrival,
        SimTime scheduled_departure,
        GateId gate,
        NodeId gate_node,
        std::vector<ServiceTask> tasks,
        std::string turnaround_id = {},
        std::optional<SimTime> target_off_block = std::nullopt);

    [[nodiscard]] AircraftId id() const noexcept;
    [[nodiscard]] const std::string& flight_number() const noexcept;
    [[nodiscard]] SimTime scheduled_arrival() const noexcept;
    [[nodiscard]] SimTime scheduled_departure() const noexcept;
    [[nodiscard]] GateId gate() const noexcept;
    [[nodiscard]] NodeId gate_node() const noexcept;
    [[nodiscard]] AircraftState state() const noexcept;
    [[nodiscard]] std::optional<SimTime> actual_arrival() const noexcept;
    [[nodiscard]] std::optional<SimTime> ready_at() const noexcept;
    [[nodiscard]] std::optional<SimTime> actual_departure() const noexcept;
    [[nodiscard]] const std::vector<ServiceTask>& tasks() const noexcept;
    [[nodiscard]] std::vector<ServiceTask>& mutable_tasks() noexcept;
    [[nodiscard]] const std::string& turnaround_id() const noexcept;
    [[nodiscard]] SimTime target_off_block() const noexcept;
    [[nodiscard]] TurnaroundState turnaround_state() const noexcept;
    [[nodiscard]] std::optional<SimTime> departure_delay() const noexcept;
    [[nodiscard]] const std::string& failure_reason() const noexcept;

    void transition_to(AircraftState next);
    void arrive(SimTime now);
    void mark_task_waiting(ServiceType type, SimTime now);
    void mark_task_waiting(TaskId id, SimTime now);
    void make_task_ready(TaskId id);
    void assign_task(ServiceType type);
    void assign_task(TaskId id, std::string resource = {}, std::optional<VehicleId> vehicle = std::nullopt);
    void requeue_task(TaskId id, SimTime now);
    void start_task(ServiceType type, SimTime now);
    void start_task(TaskId id, SimTime now);
    void complete_task(ServiceType type, SimTime now);
    void complete_task(TaskId id, SimTime now);
    void set_task_duration(TaskId id, SimTime duration);
    void fail_task(TaskId id, std::string reason);
    void fail_turnaround(std::string reason);
    void depart(SimTime now);

    [[nodiscard]] bool services_complete() const noexcept;
    [[nodiscard]] const ServiceTask& task(ServiceType type) const;
    [[nodiscard]] const ServiceTask& task(TaskId id) const;
    [[nodiscard]] ServiceTask& mutable_task(TaskId id);
    [[nodiscard]] static bool can_transition(AircraftState from, AircraftState to) noexcept;

private:
    [[nodiscard]] ServiceTask& mutable_task(ServiceType type);

    AircraftId id_;
    std::string flight_number_;
    SimTime scheduled_arrival_;
    SimTime scheduled_departure_;
    GateId gate_;
    NodeId gate_node_;
    AircraftState state_{AircraftState::Scheduled};
    std::vector<ServiceTask> tasks_;
    std::optional<SimTime> actual_arrival_;
    std::optional<SimTime> ready_at_;
    std::optional<SimTime> actual_departure_;
    std::string turnaround_id_;
    SimTime target_off_block_{};
    bool failed_{};
    std::string failure_reason_;
};

[[nodiscard]] std::string_view to_string(TurnaroundState state) noexcept;

}  // namespace airside
