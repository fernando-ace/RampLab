#include "airside/operations/simulation.hpp"
#include "airside/routing/astar.hpp"
#include "airside/scenario/scenario_loader.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <filesystem>

#include <gtest/gtest.h>

namespace airside::test {
namespace {

std::filesystem::path demo_scenario_path() {
    return std::filesystem::path{AIRSIDE_SOURCE_DIR} / "scenarios" / "ramplab_demo_airport.yaml";
}

TEST(DemoAirportTest, DefinitionIsFictionalMeterBasedAndHasFiveCompatibleStands) {
    const auto definition = YAML::LoadFile(demo_scenario_path().string());
    const auto design = definition["airport"]["design"];
    ASSERT_TRUE(design);
    EXPECT_TRUE(design["fictional"].as<bool>());
    EXPECT_EQ(design["coordinate_frame"]["units"].as<std::string>(), "meters");
    EXPECT_EQ(design["coordinate_frame"]["x_axis"].as<std::string>(), "east");
    EXPECT_EQ(design["coordinate_frame"]["y_axis"].as<std::string>(), "north");
    EXPECT_NE(design["unreal_conversion"].as<std::string>().find("Cesium"), std::string::npos);

    const auto runway = design["authored_assumptions"]["runway"];
    EXPECT_GT(runway["length_m"].as<double>(), 0.0);
    EXPECT_GT(runway["width_m"].as<double>(), 0.0);
    const auto stands = design["stands"];
    ASSERT_EQ(stands.size(), 5U);
    for (const auto& stand : stands) {
        const auto dimensions = stand["dimensions_m"];
        ASSERT_EQ(dimensions.size(), 2U);
        EXPECT_GT(dimensions[0].as<double>(), stand["compatible_wingspan_m"].as<double>());
        EXPECT_GT(dimensions[1].as<double>(), 0.0);
        EXPECT_GT(stand["wing_clearance_m"].as<double>(), 0.0);
        EXPECT_FALSE(stand["node"].as<std::string>().empty());
    }
}

TEST(DemoAirportTest, LoadedGraphHasTypedConnectivityForAircraftAndGse) {
    const auto scenario = load_scenario(demo_scenario_path());
    ASSERT_TRUE(scenario.surface_operations.has_value());
    ASSERT_EQ(scenario.gates.size(), 5U);
    ASSERT_EQ(scenario.aircraft.size(), 5U);
    ASSERT_EQ(scenario.vehicles.size(), 2U);
    ASSERT_EQ(scenario.graph.nodes().size(), 22U);
    ASSERT_GE(scenario.graph.edges().size(), 30U);

    const auto definitions = YAML::LoadFile(demo_scenario_path().string());
    const auto edges = definitions["airport"]["edges"];
    ASSERT_EQ(edges.size(), scenario.graph.edges().size());
    std::size_t gse_only_edges = 0;
    for (std::size_t index = 0; index < scenario.graph.edges().size(); ++index) {
        const auto& edge = scenario.graph.edges()[index];
        const auto& definition = edges[index];
        const auto surface_type = definition["surface_type"] ?
            definition["surface_type"].as<std::string>() : std::string{};
        if (!surface_type.empty()) {
            EXPECT_FALSE(edge.aircraft_allowed) << surface_type << " edge " << edge.id.value();
            EXPECT_TRUE(edge.ground_vehicle_allowed) << surface_type << " edge " << edge.id.value();
            EXPECT_GT(definition["width_m"].as<double>(), 0.0);
            ++gse_only_edges;
        }
        EXPECT_FALSE(edge.crosses_runway) << "edge " << edge.id.value();
        EXPECT_GT(edge.distance_m, 0.0) << "edge " << edge.id.value();
        EXPECT_GT(edge.traversal_cost.count(), 0) << "edge " << edge.id.value();
    }
    EXPECT_GE(gse_only_edges, 15U);

    const auto handoff = scenario.surface_operations->departure_handoff;
    for (const auto& gate : scenario.gates) {
        ASSERT_TRUE(find_route(scenario.graph, gate.node, handoff, MovementClass::Aircraft));
        ASSERT_TRUE(find_route(scenario.graph, handoff, gate.node, MovementClass::Aircraft));
    }
    for (const auto& flight : scenario.aircraft) {
        if (flight.operation_type() != AircraftOperationType::Turnaround) {
            ASSERT_TRUE(find_route(scenario.graph, flight.arrival_exit_node(), flight.gate_node(),
                MovementClass::Aircraft)) << flight.flight_number();
        }
    }
    for (const auto& vehicle : scenario.vehicles) {
        for (const auto& gate : scenario.gates) {
            ASSERT_TRUE(find_route(scenario.graph, vehicle.depot_node(), gate.node, MovementClass::GroundVehicle))
                << vehicle.name() << " to " << gate.name;
        }
    }
}

TEST(DemoAirportTest, CanonicalFiveAircraftRunCompletesSafelyAndRepeatsExactly) {
    const auto scenario = load_scenario(demo_scenario_path());
    const auto first = Simulation{scenario, 50}.run();
    const auto second = Simulation{scenario, 50}.run();

    EXPECT_EQ(first.events, second.events);
    EXPECT_EQ(first.metrics, second.metrics);
    EXPECT_EQ(first.metrics.total_turnarounds, 5U);
    EXPECT_EQ(first.metrics.completed_turnarounds, 5U);
    EXPECT_EQ(first.metrics.failed_or_timed_out_turnarounds, 0U);
    EXPECT_EQ(first.metrics.fleet_requests_created, 15U);
    EXPECT_EQ(first.metrics.fleet_requests_completed, 15U);
    EXPECT_EQ(first.metrics.fleet_requests_failed, 0U);
    EXPECT_EQ(first.metrics.fleet_collisions, 0U);
    EXPECT_FALSE(first.metrics.fleet_deadlock_unresolved);
    EXPECT_EQ(first.metrics.surface_total_aircraft, 5U);
    EXPECT_EQ(first.metrics.surface_arrived_aircraft, 2U);
    EXPECT_EQ(first.metrics.surface_departed_aircraft, 5U);
    EXPECT_EQ(first.metrics.surface_safe_failures, 0U);
    EXPECT_EQ(first.metrics.surface_aircraft_aircraft_collisions, 0U);
    EXPECT_EQ(first.metrics.surface_aircraft_ground_collisions, 0U);
    EXPECT_EQ(first.metrics.unresolved_service_requests, 0U);
    EXPECT_GT(first.metrics.maximum_runway_queue_depth, 1U);
}

}  // namespace
}  // namespace airside::test
