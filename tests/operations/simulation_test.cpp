#include "support/scenarios.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>

namespace airside {
namespace {

using namespace std::chrono_literals;

TEST(SimulationTest, ResourceContentionCreatesWaitingAndDepartureDelay) {
    const auto result = Simulation{test::baseline_scenario(), 42}.run();
    EXPECT_TRUE(std::ranges::any_of(result.event_log, [](const auto& entry) {
        return entry.find("AX202 waiting for fuel resource") != std::string::npos;
    }));
    EXPECT_GE(result.metrics.delayed_aircraft, 1U);
    EXPECT_GT(result.metrics.aircraft.at(2).departure_delay, 0s);
}

TEST(SimulationTest, EveryAircraftCompletesServicesBeforeDeparture) {
    const auto result = Simulation{test::baseline_scenario(), 42}.run();
    for (const auto& flight : result.aircraft) {
        EXPECT_EQ(flight.state(), AircraftState::Departed);
        EXPECT_TRUE(flight.services_complete());
        ASSERT_TRUE(flight.ready_at().has_value());
        ASSERT_TRUE(flight.actual_departure().has_value());
        EXPECT_LE(*flight.ready_at(), *flight.actual_departure());
    }
}

TEST(SimulationTest, SameSeedProducesIdenticalDomainResults) {
    Simulation first_simulation{test::baseline_scenario(), 42};
    Simulation second_simulation{test::baseline_scenario(), 42};
    const auto first = first_simulation.run();
    const auto second = second_simulation.run();
    EXPECT_EQ(first.seed, second.seed);
    EXPECT_EQ(first.simulated_duration, second.simulated_duration);
    EXPECT_EQ(first.event_log, second.event_log);
    EXPECT_EQ(first.events, second.events);
    EXPECT_EQ(first.metrics, second.metrics);
    EXPECT_EQ(first_simulation.snapshot(), second_simulation.snapshot());
}

TEST(SimulationTest, ExternalHighCapacityScenarioChangesContentionResults) {
    const auto baseline = Simulation{test::baseline_scenario(), 42}.run();
    const auto high_capacity = Simulation{test::high_capacity_scenario(), 42}.run();
    EXPECT_LT(high_capacity.metrics.delayed_aircraft, baseline.metrics.delayed_aircraft);
    EXPECT_LT(high_capacity.metrics.average_turnaround_seconds,
        baseline.metrics.average_turnaround_seconds);
    EXPECT_NE(high_capacity.events, baseline.events);
}

TEST(SimulationTest, RoadClosureReroutesLaterVehicleDispatch) {
    const auto result = Simulation{test::baseline_scenario(), 42}.run();
    const auto assignment = std::ranges::find_if(result.event_log, [](const auto& entry) {
        return entry.find("FuelTruck-1 assigned to AX202") != std::string::npos;
    });
    ASSERT_NE(assignment, result.event_log.end());
    EXPECT_NE(assignment->find("route 1->3->5"), std::string::npos);
}

}  // namespace
}  // namespace airside
