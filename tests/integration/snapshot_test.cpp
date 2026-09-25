#include "support/scenarios.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>

namespace airside {
namespace {

using namespace std::chrono_literals;

TEST(SnapshotTest, CapturesActiveAircraftGateAndTimedMultiSegmentRoutes) {
    Simulation simulation{test::baseline_scenario(), 42};
    ASSERT_TRUE(simulation.advance());
    const auto snapshot = simulation.snapshot();

    EXPECT_EQ(snapshot.version, kSnapshotSchemaVersion);
    EXPECT_EQ(snapshot.simulation_time, 0s);
    ASSERT_EQ(snapshot.aircraft.size(), 3U);
    EXPECT_EQ(snapshot.aircraft[0].state, AircraftState::WaitingForServices);
    EXPECT_EQ(snapshot.aircraft[0].logical_node, NodeId{4});
    EXPECT_EQ(snapshot.aircraft[1].state, AircraftState::Scheduled);

    ASSERT_EQ(snapshot.gates.size(), 3U);
    EXPECT_EQ(snapshot.gates[0].occupying_aircraft, AircraftId{1});
    EXPECT_FALSE(snapshot.gates[0].available);
    EXPECT_EQ(snapshot.gates[0].position_m, (Vec2{220.0, 80.0}));

    ASSERT_EQ(snapshot.vehicles.size(), 2U);
    const auto& fuel = snapshot.vehicles[0];
    EXPECT_EQ(fuel.state, VehicleState::TravelingToAircraft);
    EXPECT_EQ(fuel.current_node, NodeId{1});
    EXPECT_EQ(fuel.destination_node, NodeId{4});
    ASSERT_TRUE(fuel.journey.has_value());
    EXPECT_EQ(fuel.journey->departure_time, 0s);
    EXPECT_EQ(fuel.journey->expected_arrival_time, 3min);
    ASSERT_EQ(fuel.journey->segments.size(), 2U);
    EXPECT_EQ(fuel.journey->segments[0].departure_time, 0s);
    EXPECT_EQ(fuel.journey->segments[0].expected_arrival_time, 2min);
    EXPECT_EQ(fuel.journey->segments[1].departure_time, 2min);
    EXPECT_EQ(fuel.journey->segments[1].expected_arrival_time, 3min);
}

TEST(SnapshotTest, IsIndependentOfLaterSimulationMutationAndReflectsRoadClosure) {
    Simulation simulation{test::baseline_scenario(), 42};
    ASSERT_TRUE(simulation.advance());
    const auto retained = simulation.snapshot();
    while (!simulation.finished() && simulation.current_time() < 5min) {
        ASSERT_TRUE(simulation.advance());
    }
    const auto after_closure = simulation.snapshot();
    const auto closed = std::ranges::find_if(after_closure.roads,
        [](const auto& road) { return road.id == EdgeId{4}; });
    ASSERT_NE(closed, after_closure.roads.end());
    EXPECT_FALSE(closed->enabled);
    EXPECT_EQ(retained.simulation_time, 0s);
    EXPECT_TRUE(std::ranges::find_if(retained.roads,
        [](const auto& road) { return road.id == EdgeId{4}; })->enabled);
    EXPECT_EQ(retained.gates[0].occupying_aircraft, AircraftId{1});
}

TEST(SnapshotTest, FinalSnapshotContainsDepartedAircraftIdleVehiclesAndAvailableGates) {
    Simulation simulation{test::baseline_scenario(), 42};
    const auto result = simulation.run();
    const auto snapshot = simulation.snapshot();
    EXPECT_EQ(snapshot.simulation_time, result.simulated_duration);
    EXPECT_TRUE(std::ranges::all_of(snapshot.aircraft,
        [](const auto& flight) { return flight.state == AircraftState::Departed; }));
    EXPECT_TRUE(std::ranges::all_of(snapshot.vehicles,
        [](const auto& vehicle) { return vehicle.state == VehicleState::Idle && !vehicle.journey; }));
    EXPECT_TRUE(std::ranges::all_of(snapshot.gates,
        [](const auto& gate) { return gate.available && !gate.occupying_aircraft; }));
}

TEST(SnapshotTest, RepeatedSnapshotGenerationDoesNotMutateSimulation) {
    Simulation simulation{test::baseline_scenario(), 42};
    ASSERT_TRUE(simulation.advance());
    const auto first = simulation.snapshot();
    const auto second = simulation.snapshot();
    EXPECT_EQ(first, second);
    EXPECT_EQ(simulation.current_time(), 0s);
}

}  // namespace
}  // namespace airside
