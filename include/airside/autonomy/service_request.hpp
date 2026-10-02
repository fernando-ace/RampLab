#pragma once

#include "airside/autonomy/simulation.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace airside::autonomy {

struct VehicleId {
    std::string value;
    auto operator<=>(const VehicleId&) const = default;
};

struct ServiceRequestId {
    std::string value;
    auto operator<=>(const ServiceRequestId&) const = default;
};

enum class ServiceKind { BaggageDelivery, FuelService, AircraftTurnaround, TugCartMovement };
enum class ServiceTaskState { Queued, Assigned, EnRoute, Servicing, Completed, Failed };
enum class DispatchVehicleState { Idle, Busy, Degraded, Unavailable, Recovering };

struct ServiceRequest {
    ServiceRequestId id;
    ServiceKind kind{ServiceKind::TugCartMovement};
    std::string required_capability{"tug"};
    std::string origin;
    std::string destination;
    double release_time_s{};
    int priority{};
    std::optional<double> deadline_s;
    double service_duration_s{};
    std::vector<SensorFault> faults;
    bool operator==(const ServiceRequest&) const = default;
};

struct DispatchVehicle {
    VehicleId id;
    std::vector<std::string> capabilities;
    std::string current_node;
    DispatchVehicleState state{DispatchVehicleState::Idle};
    std::optional<ServiceRequestId> current_request;
    std::vector<SensorFault> faults;
};

struct ServiceTaskSnapshot {
    ServiceRequest request;
    ServiceTaskState state{ServiceTaskState::Queued};
    std::optional<VehicleId> assigned_vehicle;
    double released_at_s{};
    double assigned_at_s{};
    double completed_at_s{};
    double queue_wait_s{};
    std::size_t reassignments{};
    std::int64_t effective_priority{};
    bool operator==(const ServiceTaskSnapshot&) const = default;
};

enum class DispatchEventKind {
    RequestReleased,
    CandidateEvaluated,
    Assigned,
    StateChanged,
    Unassigned,
    Requeued,
    Reassigned,
    AgingApplied,
    Completed,
    Failed
};

struct DispatchEvent {
    double time_s{};
    DispatchEventKind kind{};
    ServiceRequestId request;
    VehicleId vehicle;
    std::string detail;
    std::int64_t effective_priority{};
    double route_distance_m{};
    bool operator==(const DispatchEvent&) const = default;
};

struct FleetDispatchMetrics {
    std::size_t requests_created{};
    std::size_t requests_completed{};
    std::size_t requests_failed{};
    std::size_t deadline_misses{};
    std::size_t assignments{};
    std::size_t reassignments{};
    std::size_t aging_activations{};
    double total_queue_wait_s{};
    double maximum_queue_wait_s{};
    double average_queue_wait_s{};
    double maximum_completion_time_s{};
    std::size_t unfinished_requests{};
    std::vector<DispatchEvent> events;
    std::vector<ServiceTaskSnapshot> requests;
    bool operator==(const FleetDispatchMetrics&) const = default;
};

[[nodiscard]] std::string to_string(ServiceKind kind);
[[nodiscard]] std::string to_string(ServiceTaskState state);
[[nodiscard]] std::string to_string(DispatchVehicleState state);
[[nodiscard]] std::string to_string(DispatchEventKind kind);

} // namespace airside::autonomy
