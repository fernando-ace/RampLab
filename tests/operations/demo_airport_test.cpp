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

TEST(DemoAirportTest, LoaderKeepsFictionalGeometryAndOperationalMetadata) {
    const auto scenario = load_scenario(demo_scenario_path());
    ASSERT_TRUE(scenario.airport_design.has_value());
    const auto& design = *scenario.airport_design;
    EXPECT_EQ(design.name, "RampLab Demo Airport");
    EXPECT_TRUE(design.fictional);
    EXPECT_EQ(design.coordinate_units, "meters");
    EXPECT_EQ(design.x_axis, "east");
    EXPECT_EQ(design.y_axis, "north");
    EXPECT_EQ(design.z_axis, "up");
    EXPECT_DOUBLE_EQ(design.unreal_centimeters_per_meter, 100.0);
    ASSERT_EQ(design.runways.size(), 1U);
    const auto& runway = design.runways.front();
    EXPECT_DOUBLE_EQ(runway.length_m, 1400.0);
    EXPECT_DOUBLE_EQ(runway.width_m, 30.0);
    EXPECT_EQ(runway.surface_type, "asphalt");
    ASSERT_EQ(runway.arrival_exits.size(), 2U);
    ASSERT_EQ(design.hold_points.size(), 1U);
    ASSERT_EQ(design.stands.size(), 5U);
    ASSERT_EQ(design.support_areas.size(), 4U);
    ASSERT_EQ(design.regions.size(), 4U);
    for (const auto& stand : design.stands) {
        EXPECT_GT(stand.length_m, stand.maximum_wingspan_m);
        EXPECT_GT(stand.width_m, 0.0);
        EXPECT_GT(stand.wingtip_clearance_m, 0.0);
        EXPECT_EQ(stand.footprint.size(), 4U);
        EXPECT_EQ(stand.pushback_direction, "north");
        EXPECT_FALSE(stand.tug_required);
    }
}

TEST(DemoAirportTest, LoadedGraphHasTypedConnectivityForAircraftAndGse) {
    const auto scenario = load_scenario(demo_scenario_path());
    ASSERT_TRUE(scenario.surface_operations.has_value());
    ASSERT_EQ(scenario.gates.size(), 5U);
    ASSERT_EQ(scenario.aircraft.size(), 5U);
    ASSERT_EQ(scenario.vehicles.size(), 2U);
    ASSERT_TRUE(scenario.airport_design.has_value());
    ASSERT_EQ(scenario.graph.nodes().size(), 35U);
    ASSERT_EQ(scenario.graph.edges().size(), 57U);

    const auto definitions = YAML::LoadFile(demo_scenario_path().string());
    const auto edges = definitions["airport"]["edges"];
    ASSERT_EQ(edges.size(), scenario.graph.edges().size());
    std::size_t gse_only_edges = 0;
    for (std::size_t index = 0; index < scenario.graph.edges().size(); ++index) {
        const auto& edge = scenario.graph.edges()[index];
        const auto& definition = edges[index];
        EXPECT_EQ(edge.centerline.size(), 2U) << "edge " << edge.id.value();
        EXPECT_GT(edge.width_m, 0.0) << "edge " << edge.id.value();
        EXPECT_GE(edge.minimum_clearance_m, 0.0) << "edge " << edge.id.value();
        EXPECT_NE(edge.surface_type, "unclassified") << "edge " << edge.id.value();
        EXPECT_NEAR(edge.centerline_length_m, edge.distance_m, 0.05) << "edge " << edge.id.value();
        if (edge.surface_type.starts_with("service_") || edge.surface_type == "apron_service_spur") {
            EXPECT_FALSE(edge.aircraft_allowed) << edge.surface_type << " edge " << edge.id.value();
            EXPECT_TRUE(edge.ground_vehicle_allowed) << edge.surface_type << " edge " << edge.id.value();
            ++gse_only_edges;
        }
        EXPECT_FALSE(edge.crosses_runway) << "edge " << edge.id.value();
        EXPECT_GT(edge.distance_m, 0.0) << "edge " << edge.id.value();
        EXPECT_GT(edge.traversal_cost.count(), 0) << "edge " << edge.id.value();
    }
    EXPECT_GE(gse_only_edges, 40U);

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
            ASSERT_TRUE(find_route(scenario.graph, gate.node, vehicle.depot_node(), MovementClass::GroundVehicle))
                << gate.name << " to " << vehicle.name();
        }
    }
    for (const auto& runway : scenario.airport_design->runways) {
        for (const auto& exit : runway.arrival_exits)
            for (const auto& gate : scenario.gates)
                ASSERT_TRUE(find_route(scenario.graph, exit.runway_node, gate.node, MovementClass::Aircraft));
        for (const auto& gate : scenario.gates) {
            ASSERT_TRUE(find_route(scenario.graph, gate.node, runway.departure_hold_node, MovementClass::Aircraft));
            ASSERT_TRUE(find_route(scenario.graph, runway.departure_hold_node, gate.node, MovementClass::Aircraft));
        }
    }
    const auto snapshot = Simulation{scenario, 50}.snapshot();
    EXPECT_EQ(snapshot.version, kSnapshotSchemaVersion);
    ASSERT_TRUE(snapshot.airport_design.has_value());
    EXPECT_EQ(snapshot.airport_design->runways.front().arrival_exits.size(), 2U);
    ASSERT_EQ(snapshot.roads.size(), scenario.graph.edges().size());
    EXPECT_TRUE(std::ranges::any_of(snapshot.roads, [](const auto& road) {
        return road.surface_type == "taxiway_asphalt" && road.width_m == 18.0 && road.aircraft_allowed;
    }));
    EXPECT_TRUE(std::ranges::any_of(snapshot.roads, [](const auto& road) {
        return road.surface_type == "service_road_paved" && road.width_m == 7.0 &&
            !road.aircraft_allowed && road.ground_vehicle_allowed;
    }));
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
    EXPECT_EQ(first.metrics.fleet_requests_created, 30U);
    EXPECT_EQ(first.metrics.fleet_requests_completed, 30U);
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
