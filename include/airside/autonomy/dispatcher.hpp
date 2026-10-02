#pragma once

#include "airside/autonomy/service_request.hpp"
#include "airside/world/airport_graph.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace airside::autonomy {

// Event-driven by simulation time. Aging raises a request's effective priority by one
// every aging_interval_s of waiting; ties use older release time, request ID, then vehicle ID.
class FleetDispatcher {
public:
    explicit FleetDispatcher(std::vector<ServiceRequest> requests, double aging_interval_s = 30.0);

    void add_request(ServiceRequest request);
    void update_service_duration(const ServiceRequestId& request, double duration_s, double simulation_time_s);
    [[nodiscard]] std::vector<ServiceTaskSnapshot> snapshot() const;
    [[nodiscard]] const std::vector<DispatchEvent>& events() const noexcept { return events_; }
    [[nodiscard]] bool all_terminal() const noexcept;
    [[nodiscard]] std::optional<double> next_release_time_s() const noexcept;

    void release_due(double simulation_time_s);
    [[nodiscard]] std::vector<std::pair<ServiceRequestId, VehicleId>> dispatch(
        double simulation_time_s, const std::vector<DispatchVehicle>& vehicles,
        const AirportGraph& graph);
    void set_state(const ServiceRequestId& request, ServiceTaskState state, double simulation_time_s);
    void mark_vehicle_unavailable(const VehicleId& vehicle, double simulation_time_s);
    void fail(const ServiceRequestId& request, double simulation_time_s, std::string reason);
    void fail_unfinished(double simulation_time_s, std::string reason);
    [[nodiscard]] FleetDispatchMetrics metrics() const;

private:
    struct TaskRecord {
        ServiceRequest request;
        ServiceTaskState state{ServiceTaskState::Queued};
        std::optional<VehicleId> assigned_vehicle;
        bool released{};
        double released_at_s{};
        double queued_since_s{};
        double queue_wait_s{};
        double assigned_at_s{};
        double completed_at_s{};
        std::size_t reassignments{};
        std::int64_t last_aging_level{};
    };

    [[nodiscard]] TaskRecord& find(const ServiceRequestId& request);
    [[nodiscard]] const TaskRecord& find(const ServiceRequestId& request) const;
    void emit(double time_s, DispatchEventKind kind, const TaskRecord& task,
              VehicleId vehicle = {}, std::string detail = {}, double route_distance_m = 0.0);

    std::vector<TaskRecord> tasks_;
    std::map<VehicleId, ServiceRequestId> active_assignments_;
    std::vector<DispatchEvent> events_;
    double aging_interval_s_{};
    double last_time_s_{};
    std::size_t assignments_{};
    std::size_t reassignments_{};
    std::size_t aging_activations_{};
};

} // namespace airside::autonomy
