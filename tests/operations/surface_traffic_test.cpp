#include "airside/operations/simulation.hpp"
#include "airside/operations/surface_safety.hpp"
#include "airside/scenario/scenario_loader.hpp"
#include "airside/experiment/executor.hpp"
#include "airside/experiment/experiment_loader.hpp"
#include "airside/experiment/case_generator.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <iostream>

namespace {

std::filesystem::path scenario_file(const char* name) {
    return std::filesystem::path{AIRSIDE_SOURCE_DIR} / "scenarios" / name;
}

TEST(SurfaceTrafficTest, ControlCoordinatesPushbackTaxiContentionAndRunwayQueue) {
    airside::Simulation simulation(airside::load_scenario(scenario_file("surface_traffic.yaml")), 42);
    std::size_t steps = 0;
    while (!simulation.finished() && steps < 10000) {
        (void)simulation.advance();
        ++steps;
    }
    if (!simulation.finished()) {
        const auto pending = simulation.snapshot();
        for (const auto& item : pending.aircraft)
            std::cerr << item.id.value() << ':' << item.surface_state << " node="
                      << (item.surface_node ? std::to_string(item.surface_node->value()) : "none")
                      << " wait=" << item.surface_wait_duration.count() << " reason=" << item.surface_wait_reason
                      << " pos=" << (item.surface_position_m ? std::to_string(item.surface_position_m->x_m) + "," + std::to_string(item.surface_position_m->y_m) : "none") << '\n';
        for (const auto& item : pending.vehicles)
            std::cerr << "vehicle " << item.id.value() << " pos="
                      << (item.observed_position_m ? std::to_string(item.observed_position_m->x_m) + "," + std::to_string(item.observed_position_m->y_m) : "none")
                      << " status=" << item.fleet_status << '\n';
        for (const auto& turnaround : pending.turnarounds) for (const auto& task : turnaround.tasks)
            std::cerr << "task " << task.id.value() << ' ' << airside::to_string(task.status)
                      << " assigned=" << task.assigned_resource << '\n';
        ASSERT_TRUE(simulation.finished()) << "surface run did not terminate after " << steps
            << " events at simulation second " << pending.simulation_time.count();
    }
    const auto result = simulation.run();
    const auto snapshot = simulation.snapshot();
    EXPECT_EQ(result.metrics.surface_total_aircraft, 3U);
    EXPECT_EQ(result.metrics.surface_departed_aircraft, 3U);
    EXPECT_NEAR(result.metrics.surface_departure_throughput_per_hour, 3.0 * 3600.0 / result.simulated_duration.count(), 1e-9);
    EXPECT_GT(result.metrics.surface_wait_events, 0U);
    EXPECT_GT(result.metrics.runway_queue_seconds, 0);
    EXPECT_GE(result.metrics.max_simultaneous_taxiing_aircraft, 2U);
    EXPECT_EQ(result.metrics.surface_safe_failures, 0U);
    EXPECT_EQ(result.metrics.fleet_collisions, 0U);
    EXPECT_EQ(result.metrics.surface_aircraft_aircraft_collisions, 0U);
    EXPECT_EQ(result.metrics.surface_aircraft_ground_collisions, 0U);
    EXPECT_GE(result.metrics.minimum_aircraft_separation_m, 12.0);
    EXPECT_GE(result.metrics.minimum_aircraft_ground_separation_m, 8.0);
    EXPECT_EQ(std::ranges::count_if(result.events, [](const auto& event) {
        return event.type == airside::SimulationEventType::SurfacePushbackStarted;
    }), 3);
    EXPECT_EQ(std::ranges::count_if(result.events, [](const auto& event) {
        return event.type == airside::SimulationEventType::SurfaceWaitingForPushback;
    }), 1);
    EXPECT_EQ(std::ranges::count_if(result.events, [](const auto& event) {
        return event.type == airside::SimulationEventType::SurfaceTaxiRouteAssigned && event.route.has_value();
    }), 3);
    const auto reservation_acquired = std::ranges::count_if(result.events, [](const auto& event) {
        return event.type == airside::SimulationEventType::SurfaceReservationAcquired;
    });
    const auto reservation_released = std::ranges::count_if(result.events, [](const auto& event) {
        return event.type == airside::SimulationEventType::SurfaceReservationReleased;
    });
    EXPECT_GT(reservation_acquired, 0U);
    EXPECT_EQ(reservation_released, reservation_acquired);
    EXPECT_GT(std::ranges::count_if(result.events, [](const auto& event) {
        return event.type == airside::SimulationEventType::SurfaceWaitingForTraffic;
    }), 0);
    EXPECT_EQ(std::ranges::count_if(result.events, [](const auto& event) {
        return event.type == airside::SimulationEventType::SurfaceRunwayClearance;
    }), 3);
    std::vector<airside::AircraftId> runway_queue_order;
    std::vector<airside::AircraftId> clearance_order;
    std::vector<airside::AircraftId> departure_order;
    for (const auto& event : result.events) {
        if (event.type == airside::SimulationEventType::SurfaceRunwayQueueEntered && event.aircraft)
            runway_queue_order.push_back(*event.aircraft);
        if (event.type == airside::SimulationEventType::SurfaceRunwayClearance && event.aircraft)
            clearance_order.push_back(*event.aircraft);
        if (event.type == airside::SimulationEventType::AircraftDeparted && event.aircraft)
            departure_order.push_back(*event.aircraft);
        if (event.type == airside::SimulationEventType::SurfacePushbackStarted && event.aircraft) {
            const auto ready = std::ranges::find_if(result.events, [&event](const auto& candidate) {
                return candidate.type == airside::SimulationEventType::AircraftReadyForPushback
                    && candidate.aircraft == event.aircraft;
            });
            ASSERT_NE(ready, result.events.end());
            EXPECT_LT(ready->sequence, event.sequence);
        }
    }
    EXPECT_EQ(clearance_order, runway_queue_order);
    EXPECT_EQ(departure_order, runway_queue_order);
    EXPECT_TRUE(std::ranges::all_of(snapshot.aircraft, [](const auto& item) {
        return item.surface_state == "Departed" && item.taxi_distance_m > 0.0 && item.actual_surface_departure;
    }));
}

TEST(SurfaceTrafficTest, ClosureInvalidatesRouteAndReroutesWithoutBlockingDeparture) {
    airside::Simulation simulation(airside::load_scenario(scenario_file("surface_traffic_disrupted.yaml")), 42);
    std::size_t steps = 0;
    while (!simulation.finished() && steps < 10000) {
        (void)simulation.advance();
        ++steps;
    }
    if (!simulation.finished()) {
        const auto pending = simulation.snapshot();
        for (const auto& item : pending.aircraft)
            std::cerr << item.id.value() << ':' << item.surface_state << " node="
                      << (item.surface_node ? std::to_string(item.surface_node->value()) : "none")
                      << " wait=" << item.surface_wait_duration.count() << " reason=" << item.surface_wait_reason
                      << " pos=" << (item.surface_position_m ? std::to_string(item.surface_position_m->x_m) + "," + std::to_string(item.surface_position_m->y_m) : "none") << '\n';
        for (const auto& item : pending.vehicles)
            std::cerr << "vehicle " << item.id.value() << " pos="
                      << (item.observed_position_m ? std::to_string(item.observed_position_m->x_m) + "," + std::to_string(item.observed_position_m->y_m) : "none")
                      << " status=" << item.fleet_status << '\n';
        for (const auto& turnaround : pending.turnarounds) for (const auto& task : turnaround.tasks)
            std::cerr << "task " << task.id.value() << ' ' << airside::to_string(task.status)
                      << " assigned=" << task.assigned_resource << '\n';
        ASSERT_TRUE(simulation.finished()) << "disrupted surface run did not terminate after " << steps
            << " events at simulation second " << pending.simulation_time.count();
    }
    const auto result = simulation.run();
    airside::Simulation control_simulation(airside::load_scenario(scenario_file("surface_traffic.yaml")), 42);
    const auto control_result = control_simulation.run();
    EXPECT_EQ(result.metrics.surface_departed_aircraft, 3U);
    EXPECT_EQ(result.metrics.surface_safe_failures, 0U);
    EXPECT_GE(result.metrics.surface_reroutes, 1U);
    EXPECT_TRUE(std::ranges::any_of(result.events, [](const auto& event) {
        return event.type == airside::SimulationEventType::SurfaceRouteInvalidated;
    }));
    EXPECT_TRUE(std::ranges::any_of(result.events, [](const auto& event) {
        return event.type == airside::SimulationEventType::SurfaceRerouted && event.route.has_value();
    }));
    EXPECT_EQ(result.metrics.fleet_collisions, 0U);
    const auto disrupted_ax202 = std::ranges::find(result.metrics.aircraft, "AX202",
        &airside::AircraftMetrics::flight_number);
    const auto control_ax202 = std::ranges::find(control_result.metrics.aircraft, "AX202",
        &airside::AircraftMetrics::flight_number);
    ASSERT_NE(disrupted_ax202, result.metrics.aircraft.end());
    ASSERT_NE(control_ax202, control_result.metrics.aircraft.end());
    EXPECT_GE((disrupted_ax202->departure_delay - control_ax202->departure_delay).count(), 30);
}

TEST(SurfaceTrafficTest, RepeatedSeedProducesIdenticalSurfaceEventsAndMetrics) {
    auto first_scenario = airside::load_scenario(scenario_file("surface_traffic_disrupted.yaml"));
    auto second_scenario = airside::load_scenario(scenario_file("surface_traffic_disrupted.yaml"));
    airside::Simulation first(std::move(first_scenario), 42);
    airside::Simulation second(std::move(second_scenario), 42);
    const auto a = first.run();
    const auto b = second.run();
    EXPECT_EQ(a.events, b.events);
    EXPECT_EQ(a.metrics.surface_wait_seconds, b.metrics.surface_wait_seconds);
    EXPECT_EQ(a.metrics.surface_reroutes, b.metrics.surface_reroutes);
    EXPECT_EQ(a.simulated_duration, b.simulated_duration);
}

TEST(SurfaceTrafficTest, SeedsFortyTwoThroughFortyFourMatchSerialAndParallelExperiments) {
    const auto definition = airside::experiment::load_experiment(
        std::filesystem::path{AIRSIDE_SOURCE_DIR} / "experiments" / "surface_traffic_validation.yaml");
    const auto cases = airside::experiment::generate_cases(definition);
    const auto requests = airside::experiment::generate_runs(definition, cases);
    const auto scenario = airside::load_scenario(definition.scenario_path);
    const auto serial = airside::experiment::execute_runs(scenario, requests, 1);
    const auto parallel = airside::experiment::execute_runs(scenario, requests, 3);
    ASSERT_EQ(serial.runs.size(), 3U);
    ASSERT_EQ(parallel.runs.size(), serial.runs.size());
    for (std::size_t i = 0; i < serial.runs.size(); ++i) {
        const auto& a = serial.runs[i];
        const auto& b = parallel.runs[i];
        EXPECT_EQ(a.seed, 42U + i);
        EXPECT_EQ(a.seed, b.seed);
        EXPECT_EQ(a.simulated_duration_seconds, b.simulated_duration_seconds);
        EXPECT_EQ(a.average_departure_delay_seconds, b.average_departure_delay_seconds);
        EXPECT_EQ(a.surface_departed_aircraft, b.surface_departed_aircraft);
        EXPECT_EQ(a.surface_departure_throughput_per_hour, b.surface_departure_throughput_per_hour);
        EXPECT_EQ(a.surface_departed_aircraft, a.surface_total_aircraft);
        EXPECT_EQ(a.surface_wait_events, b.surface_wait_events);
        EXPECT_EQ(a.surface_wait_seconds, b.surface_wait_seconds);
        EXPECT_EQ(a.surface_taxi_distance_m, b.surface_taxi_distance_m);
        EXPECT_EQ(a.runway_queue_seconds, b.runway_queue_seconds);
        EXPECT_EQ(a.max_simultaneous_taxiing_aircraft, b.max_simultaneous_taxiing_aircraft);
        EXPECT_EQ(a.surface_safe_failures, 0U);
        EXPECT_EQ(a.surface_aircraft_aircraft_collisions, 0U);
        EXPECT_EQ(a.surface_aircraft_ground_collisions, 0U);
        EXPECT_EQ(a.minimum_aircraft_separation_m, b.minimum_aircraft_separation_m);
        EXPECT_EQ(a.minimum_aircraft_ground_separation_m, b.minimum_aircraft_ground_separation_m);
        EXPECT_EQ(a.fleet_collisions, 0U);
    }
}

TEST(SurfaceTrafficTest, DisconnectedDepartureHandoffSafelyFailsAtGate) {
    auto scenario = airside::load_scenario(scenario_file("surface_traffic.yaml"));
    const auto isolated = airside::NodeId{999};
    scenario.graph.add_node({isolated, "Isolated departure handoff", {10000.0, 10000.0}});
    scenario.surface_operations->departure_handoff = isolated;
    airside::Simulation simulation(std::move(scenario), 42);
    const auto result = simulation.run();
    EXPECT_EQ(result.metrics.surface_departed_aircraft, 0U);
    EXPECT_EQ(result.metrics.surface_safe_failures, 3U);
    EXPECT_TRUE(std::ranges::all_of(result.aircraft, [](const auto& flight) {
        return !flight.actual_departure();
    }));
}

TEST(SurfaceTrafficTest, CollisionDetectorsPreserveHoldAndCollisionThresholds) {
    EXPECT_TRUE(airside::surface_safety::aircraft_separation(19.9).hold);
    EXPECT_FALSE(airside::surface_safety::aircraft_separation(19.9).collision);
    EXPECT_TRUE(airside::surface_safety::aircraft_separation(11.9).collision);
    EXPECT_FALSE(airside::surface_safety::aircraft_separation(20.0).hold);
    EXPECT_TRUE(airside::surface_safety::aircraft_ground_separation(29.9).hold);
    EXPECT_FALSE(airside::surface_safety::aircraft_ground_separation(29.9).collision);
    EXPECT_TRUE(airside::surface_safety::aircraft_ground_separation(7.9).collision);
    EXPECT_FALSE(airside::surface_safety::aircraft_ground_separation(30.0).hold);
}

}  // namespace
