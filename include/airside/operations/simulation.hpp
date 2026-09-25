#pragma once

#include "airside/agents/aircraft.hpp"
#include "airside/agents/service_vehicle.hpp"
#include "airside/core/event_queue.hpp"
#include "airside/metrics/metrics.hpp"
#include "airside/operations/resource_pool.hpp"
#include "airside/world/airport_graph.hpp"

#include <cstdint>
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
    AirportGraph graph;
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
    SimulationMetrics metrics;

};

class Simulation {
public:
    Simulation(Scenario scenario, std::uint64_t seed, bool verbose);
    [[nodiscard]] SimulationResult run();

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
    void record(std::string message);

    [[nodiscard]] Aircraft& aircraft(AircraftId id);
    [[nodiscard]] ServiceVehicle& vehicle(VehicleId id);
    [[nodiscard]] ResourcePool& pool(ServiceType type);
    [[nodiscard]] SimTime duration(ServiceType type) const;

    Scenario scenario_;
    std::uint64_t seed_;
    bool verbose_;
    std::mt19937_64 random_;
    EventQueue events_;
    SimTime now_{};
    ResourcePool fuel_pool_;
    ResourcePool baggage_pool_;
    std::vector<std::string> log_;
};

[[nodiscard]] std::string format_sim_time(SimTime time);

}  // namespace airside
