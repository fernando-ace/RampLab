#include "airside/world/baseline_scenario.hpp"

#include <chrono>

namespace airside {

Scenario make_baseline_scenario(bool include_road_disruption) {
    using namespace std::chrono_literals;

    Scenario scenario;
    scenario.name = "baseline";
    scenario.default_seed = 42;
    scenario.graph.add_node({NodeId{1}, "Service Depot", {0.0, 0.0}});
    scenario.graph.add_node({NodeId{2}, "North Junction", {100.0, 100.0}});
    scenario.graph.add_node({NodeId{3}, "South Junction", {100.0, -100.0}});
    scenario.graph.add_node({NodeId{4}, "Gate A1", {220.0, 80.0}});
    scenario.graph.add_node({NodeId{5}, "Gate A2", {240.0, 0.0}});
    scenario.graph.add_node({NodeId{6}, "Gate A3", {220.0, -80.0}});

    scenario.graph.add_edge({EdgeId{1}, NodeId{1}, NodeId{2}, 141.4, 2min, true});
    scenario.graph.add_edge({EdgeId{2}, NodeId{1}, NodeId{3}, 141.4, 2min, true});
    scenario.graph.add_edge({EdgeId{3}, NodeId{2}, NodeId{4}, 121.7, 1min, true});
    scenario.graph.add_edge({EdgeId{4}, NodeId{2}, NodeId{5}, 172.0, 2min, true});
    scenario.graph.add_edge({EdgeId{5}, NodeId{3}, NodeId{5}, 172.0, 2min, true});
    scenario.graph.add_edge({EdgeId{6}, NodeId{3}, NodeId{6}, 121.7, 1min, true});
    scenario.graph.add_edge({EdgeId{7}, NodeId{2}, NodeId{3}, 200.0, 3min, true});
    scenario.graph.add_edge({EdgeId{8}, NodeId{4}, NodeId{5}, 101.9, 1min, true});
    scenario.graph.add_edge({EdgeId{9}, NodeId{5}, NodeId{6}, 101.9, 1min, true});

    scenario.gates.push_back({GateId{1}, "A1", NodeId{4}});
    scenario.gates.push_back({GateId{2}, "A2", NodeId{5}});
    scenario.gates.push_back({GateId{3}, "A3", NodeId{6}});

    scenario.aircraft.emplace_back(
        AircraftId{1}, "AX101", 0min, 25min, GateId{1}, NodeId{4},
        std::vector<ServiceTask>{{TaskId{1}, ServiceType::Fueling}, {TaskId{2}, ServiceType::Baggage}});
    scenario.aircraft.emplace_back(
        AircraftId{2}, "AX202", 4min, 30min, GateId{2}, NodeId{5},
        std::vector<ServiceTask>{{TaskId{3}, ServiceType::Fueling}, {TaskId{4}, ServiceType::Baggage}});
    scenario.aircraft.emplace_back(
        AircraftId{3}, "AX303", 8min, 35min, GateId{3}, NodeId{6},
        std::vector<ServiceTask>{{TaskId{5}, ServiceType::Fueling}, {TaskId{6}, ServiceType::Baggage}});

    scenario.vehicles.emplace_back(VehicleId{1}, "FuelTruck-1", ServiceType::Fueling, NodeId{1});
    scenario.vehicles.emplace_back(VehicleId{2}, "BaggageCart-1", ServiceType::Baggage, NodeId{1});
    scenario.service_durations.emplace(ServiceType::Fueling, 8min);
    scenario.service_durations.emplace(ServiceType::Baggage, 10min);
    if (include_road_disruption) {
        scenario.road_events.push_back({5min, EdgeId{4}, false});
    }
    return scenario;
}

}  // namespace airside
