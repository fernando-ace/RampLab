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

enum class ServiceType { Fueling, Baggage };
enum class TaskStatus { Pending, Waiting, Assigned, InProgress, Completed };

struct ServiceTask {
    TaskId id;
    ServiceType type;
    TaskStatus status{TaskStatus::Pending};
    std::optional<SimTime> requested_at;
    std::optional<SimTime> started_at;
    std::optional<SimTime> completed_at;
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
        std::vector<ServiceTask> tasks);

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

    void transition_to(AircraftState next);
    void arrive(SimTime now);
    void mark_task_waiting(ServiceType type, SimTime now);
    void assign_task(ServiceType type);
    void start_task(ServiceType type, SimTime now);
    void complete_task(ServiceType type, SimTime now);
    void depart(SimTime now);

    [[nodiscard]] bool services_complete() const noexcept;
    [[nodiscard]] const ServiceTask& task(ServiceType type) const;
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
};

}  // namespace airside
