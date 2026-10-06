#pragma once

#include "airside/agents/aircraft.hpp"
#include "airside/agents/service_vehicle.hpp"
#include "airside/core/event_queue.hpp"
#include "airside/core/event_stream.hpp"
#include "airside/metrics/metrics.hpp"
#include "airside/integration/snapshot.hpp"
#include "airside/operations/resource_pool.hpp"
#include "airside/world/airport_graph.hpp"
#include "airside/world/gate.hpp"

#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace airside {

namespace autonomy { class FleetSimulation; }

struct RoadAvailabilityEvent {
    SimTime time;
    EdgeId edge;
    bool available;
};

struct TaskDurationDisruption {
    SimTime time;
    TaskId task;
    SimTime duration;
};

struct VehicleOutageEvent {
    SimTime time;
    VehicleId vehicle;
};

struct SurfaceOperationsConfig {
    NodeId departure_handoff;
    NodeId runway_node;
    NodeId arrival_exit;
    SimTime pushback_duration{SimTime{30}};
    SimTime runway_occupancy{SimTime{60}};
    SimTime arrival_rollout{SimTime{90}};
    double aircraft_speed_mps{5.0};
    double departure_queue_spacing_m{30.0};
};

struct Scenario {
    std::string name{"unnamed"};
    std::uint64_t default_seed{42};
    AirportGraph graph;
    std::vector<Gate> gates;
    std::vector<Aircraft> aircraft;
    std::vector<ServiceVehicle> vehicles;
    std::unordered_map<ServiceType, SimTime> service_durations;
    std::unordered_map<ServiceType, std::size_t> abstract_resource_capacity;
    bool turnaround_orchestration{false};
    std::vector<RoadAvailabilityEvent> road_events;
    std::vector<TaskDurationDisruption> task_duration_disruptions;
    std::vector<VehicleOutageEvent> vehicle_outages;
    std::optional<SurfaceOperationsConfig> surface_operations;
};

struct SimulationResult {
    std::uint64_t seed{0};
    SimTime simulated_duration{};
    std::vector<Aircraft> aircraft;
    std::vector<ServiceVehicle> vehicles;
    std::vector<std::string> event_log;
    std::vector<SimulationEventRecord> events;
    SimulationMetrics metrics;

};

enum class SimulationHistoryPolicy { Retain, Discard };

class Simulation {
public:
    Simulation(
        Scenario scenario,
        std::uint64_t seed,
        SimulationHistoryPolicy history_policy = SimulationHistoryPolicy::Retain);
    ~Simulation();
    Simulation(const Simulation&) = delete;
    Simulation& operator=(const Simulation&) = delete;
    void add_event_sink(ISimulationEventSink& sink);
    [[nodiscard]] bool finished() const noexcept;
    [[nodiscard]] std::optional<SimTime> next_event_time() const noexcept;
    [[nodiscard]] bool advance();
    [[nodiscard]] SimulationResult run();
    [[nodiscard]] SimulationResult result() const;
    [[nodiscard]] SimTime current_time() const noexcept;
    [[nodiscard]] const std::vector<SimulationEventRecord>& event_history() const noexcept;
    [[nodiscard]] SimulationSnapshot snapshot() const;

private:
    void process(const Event& event);
    void handle_aircraft_arrival(AircraftId id);
    void handle_vehicle_arrival(VehicleId id);
    void handle_service_completed(VehicleId id);
    void handle_vehicle_return(VehicleId id);
    void handle_road_event(EdgeId id, bool available);
    void handle_departure(AircraftId id);
    void handle_abstract_service_completed(AircraftId id, TaskId task);
    void handle_task_eligibility(AircraftId id, TaskId task);
    void handle_task_duration_change(TaskId task, SimTime duration);
    void handle_vehicle_outage(VehicleId vehicle);
    void handle_surface_tick();
    void request_surface_departure(AircraftId aircraft);
    void finish_surface_departure(AircraftId aircraft);
    void handle_fleet_tick();
    void schedule_turnaround_tasks(Aircraft& aircraft);
    void complete_turnaround_task(Aircraft& aircraft, TaskId task);
    void update_turnaround_estimate(const Aircraft& aircraft);
    void queue_mobile_task(Aircraft& aircraft, ServiceTask& task, std::int64_t priority);
    void synchronize_fleet_state();
    void schedule_fleet_tick();
    void request_service(Aircraft& aircraft, ServiceType type);
    void dispatch(VehicleId vehicle_id, AircraftId aircraft_id);
    void emit(SimulationEventRecord event);
    void emit_aircraft_state(Aircraft& aircraft, AircraftState previous);
    void emit_vehicle_state(ServiceVehicle& vehicle, VehicleState previous);

    [[nodiscard]] Aircraft& aircraft(AircraftId id);
    [[nodiscard]] ServiceVehicle& vehicle(VehicleId id);
    [[nodiscard]] ResourcePool& pool(ServiceType type);
    [[nodiscard]] SimTime duration(ServiceType type) const;
    [[nodiscard]] Gate& gate(GateId id);
    [[nodiscard]] const Gate& gate(GateId id) const;

    Scenario scenario_;
    std::uint64_t seed_;
    std::mt19937_64 random_;
    EventQueue events_;
    SimTime now_{};
    ResourcePool fuel_pool_;
    ResourcePool baggage_pool_;
    std::vector<std::string> log_;
    std::vector<SimulationEventRecord> event_history_;
    std::vector<ISimulationEventSink*> event_sinks_;
    std::uint64_t next_event_record_sequence_{0};
    SimulationHistoryPolicy history_policy_{SimulationHistoryPolicy::Retain};
    std::unique_ptr<autonomy::FleetSimulation> autonomy_fleet_;
    std::map<std::string, std::pair<AircraftId, TaskId>> fleet_task_requests_;
    bool fleet_tick_scheduled_{false};
    std::unordered_map<ServiceType, std::size_t> abstract_resources_in_use_;
    std::unordered_map<ServiceType, std::uint64_t> task_replan_counts_;
    std::unordered_map<AircraftId, std::vector<TaskId>> critical_paths_;
    std::unordered_map<AircraftId, SimTime> estimated_ready_times_;
    std::unordered_map<AircraftId, SimTime> schedule_slacks_;
    std::unordered_map<AircraftId, bool> predicted_late_;
    std::uint64_t task_reassignments_{0};
    std::uint64_t disruption_replans_{0};
    struct SurfaceAircraftState {
        enum class Phase { None, ArrivalQueue, WaitingForPushback, Pushback, Taxiing, WaitingForTraffic, WaitingForRunway, Runway, Arrived, Departed, Failed };
        Phase phase{Phase::None};
        NodeId node{};
        std::vector<NodeId> route_nodes;
        std::vector<EdgeId> route_edges;
        std::size_t edge_index{};
        std::optional<SimTime> phase_end;
        std::optional<SimTime> phase_started;
        std::optional<SimTime> wait_started;
        SimTime queued_at{};
        std::size_t queue_slot{};
        SimTime accumulated_wait{};
        bool pushback_wait_reported{false};
        bool clearance_waiting{false};
        bool arrival_operation{false};
        std::optional<SimTime> pushback_started_at;
        std::optional<SimTime> pushback_completed_at;
        std::optional<SimTime> taxi_started_at;
        std::optional<SimTime> taxi_completed_at;
        std::optional<SimTime> runway_queue_entered_at;
        std::optional<SimTime> runway_clearance_at;
        std::optional<SimTime> runway_release_at;
        std::optional<SimTime> arrival_gate_at;
        SimTime runway_wait{};
        SimTime runway_occupied{};
        std::optional<SimTime> actual_surface_departure;
        double taxi_distance_m{};
        std::size_t reroutes{};
    };
    std::map<AircraftId, SurfaceAircraftState> surface_aircraft_;
    std::map<EdgeId, AircraftId> surface_edge_reservations_;
    std::map<NodeId, AircraftId> surface_node_reservations_;
    SimTime runway_available_at_{};
    bool surface_tick_scheduled_{false};
    std::size_t maximum_simultaneous_taxiing_{};
    std::size_t surface_wait_events_{};
    std::size_t next_surface_queue_slot_{};
    std::size_t maximum_runway_queue_depth_{};
    double minimum_aircraft_separation_m_{std::numeric_limits<double>::infinity()};
    double minimum_aircraft_ground_separation_m_{std::numeric_limits<double>::infinity()};
    std::size_t surface_aircraft_aircraft_collisions_{};
    std::size_t surface_aircraft_ground_collisions_{};
};

[[nodiscard]] std::string format_sim_time(SimTime time);

}  // namespace airside
