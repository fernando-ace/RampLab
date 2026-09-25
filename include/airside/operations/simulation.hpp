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
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace airside {

struct RoadAvailabilityEvent {
    SimTime time;
    EdgeId edge;
    bool available;
};

struct Scenario {
    std::string name{"unnamed"};
    std::uint64_t default_seed{42};
    AirportGraph graph;
    std::vector<Gate> gates;
    std::vector<Aircraft> aircraft;
    std::vector<ServiceVehicle> vehicles;
    std::unordered_map<ServiceType, SimTime> service_durations;
    std::vector<RoadAvailabilityEvent> road_events;
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

class Simulation {
public:
    Simulation(Scenario scenario, std::uint64_t seed);
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
};

[[nodiscard]] std::string format_sim_time(SimTime time);

}  // namespace airside
