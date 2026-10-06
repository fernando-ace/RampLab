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
#include <optional>

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

TEST(SurfaceTrafficTest, ArrivalTurnaroundContinuesThroughPushbackAndDeparture) {
    airside::Scenario scenario;
    scenario.name = "arrival_turnaround_regression";
    scenario.turnaround_orchestration = true;
    const airside::NodeId runway{1}, exit{2}, gate_node{3}, handoff{4};
    scenario.graph.add_node({runway, "runway", {0.0, 0.0}});
    scenario.graph.add_node({exit, "exit", {100.0, 0.0}});
    scenario.graph.add_node({gate_node, "gate", {200.0, 0.0}});
    scenario.graph.add_node({handoff, "handoff", {300.0, 0.0}});
    const airside::NodeId depot{5};
    scenario.graph.add_node({depot, "depot", {1000.0, 1000.0}});
    scenario.graph.add_edge({airside::EdgeId{1}, exit, gate_node, 100.0, airside::SimTime{1}, true, false});
    scenario.graph.add_edge({airside::EdgeId{2}, gate_node, handoff, 100.0, airside::SimTime{1}, true, false});
    const airside::GateId stand{1};
    scenario.gates.emplace_back(stand, "A1", gate_node);
    scenario.vehicles.emplace_back(airside::VehicleId{1}, "Fuel-1", airside::ServiceType::Fueling, depot);
    scenario.vehicles.emplace_back(airside::VehicleId{2}, "Baggage-1", airside::ServiceType::Baggage, depot);
    airside::ServiceTask deboard{airside::TaskId{1}, airside::ServiceType::Deboarding};
    deboard.duration = airside::SimTime{2};
    airside::ServiceTask prepare{airside::TaskId{2}, airside::ServiceType::PushbackPreparation};
    prepare.duration = airside::SimTime{2};
    prepare.prerequisites.push_back(deboard.id);
    scenario.aircraft.emplace_back(airside::AircraftId{1}, "INB001", airside::SimTime{0},
        airside::SimTime{10}, stand, gate_node, std::vector<airside::ServiceTask>{deboard, prepare},
        "TO-INB001", airside::SimTime{8}, airside::AircraftOperationType::ArrivalTurnaround, exit);
    airside::ServiceTask second_deboard{airside::TaskId{3}, airside::ServiceType::Deboarding};
    second_deboard.duration = airside::SimTime{2};
    airside::ServiceTask second_prepare{airside::TaskId{4}, airside::ServiceType::PushbackPreparation};
    second_prepare.duration = airside::SimTime{2};
    second_prepare.prerequisites.push_back(second_deboard.id);
    scenario.aircraft.emplace_back(airside::AircraftId{2}, "INB002", airside::SimTime{1},
        airside::SimTime{11}, stand, gate_node,
        std::vector<airside::ServiceTask>{second_deboard, second_prepare}, "TO-INB002",
        airside::SimTime{9}, airside::AircraftOperationType::ArrivalTurnaround, exit);
    scenario.service_durations.emplace(airside::ServiceType::Fueling, airside::SimTime{1});
    scenario.service_durations.emplace(airside::ServiceType::Baggage, airside::SimTime{1});
    scenario.abstract_resource_capacity.emplace(airside::ServiceType::Deboarding, 1U);
    scenario.abstract_resource_capacity.emplace(airside::ServiceType::PushbackPreparation, 1U);
    scenario.surface_operations = airside::SurfaceOperationsConfig{
        handoff, runway, exit, airside::SimTime{1}, airside::SimTime{3}, airside::SimTime{2}, 100.0, 100.0};

    airside::Simulation simulation{std::move(scenario), 42};
    std::size_t steps = 0;
    while (!simulation.finished() && steps < 1000) {
        try { (void)simulation.advance(); }
        catch (const std::exception& error) {
            const auto failed = simulation.snapshot();
            for (const auto& item : failed.aircraft)
                std::cerr << "integrated debug " << item.flight_number << " state=" << static_cast<int>(item.state)
                          << " surface=" << item.surface_state << " time=" << failed.simulation_time.count()
                          << " error=" << error.what() << '\n';
            throw;
        }
        ++steps;
    }
    ASSERT_TRUE(simulation.finished()) << "integrated arrival remained in "
        << simulation.snapshot().aircraft.front().surface_state << " at simulated second "
        << simulation.current_time().count();
    const auto result = simulation.result();
    const auto snapshot = simulation.snapshot();
    ASSERT_EQ(snapshot.aircraft.size(), 2U);
    EXPECT_TRUE(std::ranges::all_of(snapshot.aircraft, [](const auto& item) {
        return item.operation_type == "arrival_turnaround" && item.surface_state == "Departed" &&
            item.state == airside::AircraftState::Departed && item.actual_arrival && item.actual_departure;
    }));
    EXPECT_EQ(snapshot.gates.front().occupying_aircraft, std::nullopt);
    const auto find_event = [&](airside::SimulationEventType type) {
        return std::ranges::find(result.events, type, &airside::SimulationEventRecord::type);
    };
    const auto taxi_in = find_event(airside::SimulationEventType::ArrivalTaxiInStarted);
    const auto landed = find_event(airside::SimulationEventType::AircraftLanded);
    const auto gate_assigned = find_event(airside::SimulationEventType::GateAssigned);
    const auto gate_arrival = find_event(airside::SimulationEventType::ArrivalAtGate);
    const auto turnaround = find_event(airside::SimulationEventType::TurnaroundStarted);
    const auto pushback = find_event(airside::SimulationEventType::PushbackTaxiOutStarted);
    const auto departed = find_event(airside::SimulationEventType::AircraftDeparted);
    ASSERT_NE(taxi_in, result.events.end()); ASSERT_NE(gate_arrival, result.events.end());
    ASSERT_NE(landed, result.events.end()); ASSERT_NE(gate_assigned, result.events.end());
    ASSERT_NE(turnaround, result.events.end()); ASSERT_NE(pushback, result.events.end());
    ASSERT_NE(departed, result.events.end());
    const auto runway_queue = std::ranges::find_if(result.events, [&](const auto& event) {
        return event.type == airside::SimulationEventType::SurfaceRunwayQueueEntered &&
            event.sequence > pushback->sequence;
    });
    ASSERT_NE(runway_queue, result.events.end());
    EXPECT_LT(taxi_in->sequence, gate_arrival->sequence);
    EXPECT_LT(landed->sequence, taxi_in->sequence);
    EXPECT_LT(gate_assigned->sequence, gate_arrival->sequence);
    EXPECT_NE(airside::format_event(*landed).find("landed"), std::string::npos);
    EXPECT_NE(airside::format_event(*gate_assigned).find("Gate A"), std::string::npos);
    EXPECT_LT(gate_arrival->sequence, turnaround->sequence);
    EXPECT_LT(turnaround->sequence, pushback->sequence);
    EXPECT_LT(pushback->sequence, runway_queue->sequence);
    EXPECT_LT(runway_queue->sequence, departed->sequence);
    EXPECT_EQ(result.metrics.surface_departed_aircraft, 2U);
    EXPECT_EQ(result.metrics.surface_arrived_aircraft, 2U);
    EXPECT_EQ(result.metrics.gate_assignments, 2U);
    EXPECT_EQ(result.metrics.runway_operations_completed, 4U);
    EXPECT_GT(result.metrics.gate_wait_seconds, 0);
    EXPECT_GT(result.metrics.gate_occupancy_seconds, 0);
    EXPECT_GT(result.metrics.arrival_to_departure_seconds, 0);
    EXPECT_GT(result.metrics.arrival_taxi_distance_m, 0.0);
    EXPECT_GT(result.metrics.departure_taxi_distance_m, 0.0);
    EXPECT_EQ(result.metrics.surface_aircraft_aircraft_collisions, 0U);
    EXPECT_EQ(result.metrics.surface_aircraft_ground_collisions, 0U);
    EXPECT_EQ(result.metrics.surface_safe_failures, 0U);
    EXPECT_EQ(result.metrics.surface_departed_aircraft, 2U);
    EXPECT_EQ(std::ranges::count_if(result.events, [](const auto& event) {
        return event.type == airside::SimulationEventType::GateWaitStarted;
    }), 1);
    EXPECT_EQ(std::ranges::count_if(result.events, [](const auto& event) {
        return event.type == airside::SimulationEventType::AircraftDeparted;
    }), 2);
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

TEST(SurfaceTrafficTest, MixedRunwaySeedsFortyTwoThroughFortyFourMatchSerialAndParallel) {
    const auto definition = airside::experiment::load_experiment(
        std::filesystem::path{AIRSIDE_SOURCE_DIR} / "experiments" / "mixed_runway_operations_validation.yaml");
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
        EXPECT_EQ(a.surface_arrived_aircraft, b.surface_arrived_aircraft);
        EXPECT_EQ(a.surface_departed_aircraft, b.surface_departed_aircraft);
        EXPECT_EQ(a.runway_operations_completed, b.runway_operations_completed);
        EXPECT_EQ(a.maximum_runway_queue_depth, b.maximum_runway_queue_depth);
        EXPECT_EQ(a.arrival_runway_wait_seconds, b.arrival_runway_wait_seconds);
        EXPECT_EQ(a.departure_runway_wait_seconds, b.departure_runway_wait_seconds);
        EXPECT_EQ(a.average_runway_wait_seconds, b.average_runway_wait_seconds);
        EXPECT_EQ(a.runway_utilization, b.runway_utilization);
        EXPECT_EQ(a.arrival_taxi_distance_m, b.arrival_taxi_distance_m);
        EXPECT_EQ(a.departure_taxi_distance_m, b.departure_taxi_distance_m);
        EXPECT_EQ(a.arrival_taxi_seconds, b.arrival_taxi_seconds);
        EXPECT_EQ(a.departure_taxi_seconds, b.departure_taxi_seconds);
        EXPECT_EQ(a.surface_aircraft_aircraft_collisions, 0U);
        EXPECT_EQ(a.surface_aircraft_ground_collisions, 0U);
        EXPECT_EQ(a.surface_safe_failures, 0U);
        EXPECT_EQ(a.fleet_outstanding_reservations, 0U);
        EXPECT_EQ(a.fleet_collisions, 0U);
    }
}

TEST(SurfaceTrafficTest, IntegratedLifecycleSeedsFortyTwoThroughFortyFourMatchSerialAndParallel) {
    const auto definition = airside::experiment::load_experiment(
        std::filesystem::path{AIRSIDE_SOURCE_DIR} / "experiments" / "turnaround_lifecycle_validation.yaml");
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
        EXPECT_EQ(a.completed_turnarounds, 3U);
        EXPECT_EQ(a.completed_turnarounds, b.completed_turnarounds);
        EXPECT_EQ(a.surface_arrived_aircraft, 2U);
        EXPECT_EQ(a.surface_arrived_aircraft, b.surface_arrived_aircraft);
        EXPECT_EQ(a.surface_departed_aircraft, 3U);
        EXPECT_EQ(a.runway_operations_completed, 5U);
        EXPECT_EQ(a.runway_operations_completed, b.runway_operations_completed);
        EXPECT_EQ(a.gate_wait_seconds, b.gate_wait_seconds);
        EXPECT_EQ(a.gate_occupancy_seconds, b.gate_occupancy_seconds);
        EXPECT_EQ(a.arrival_to_departure_seconds, b.arrival_to_departure_seconds);
        EXPECT_EQ(a.surface_aircraft_aircraft_collisions, 0U);
        EXPECT_EQ(a.surface_aircraft_ground_collisions, 0U);
        EXPECT_EQ(a.fleet_collisions, 0U);
        EXPECT_EQ(a.minimum_aircraft_ground_separation_m, b.minimum_aircraft_ground_separation_m);
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

TEST(SurfaceTrafficTest, MixedArrivalDisruptionCompletesInboundAndOutboundAircraft) {
    auto scenario = airside::load_scenario(scenario_file("mixed_runway_disrupted.yaml"));
    const auto detour = std::ranges::find(scenario.graph.nodes(), std::string{"A4 Taxi Detour"},
        &airside::AirportNode::name);
    const auto merge = std::ranges::find(scenario.graph.nodes(), std::string{"A4 Taxi Merge"},
        &airside::AirportNode::name);
    const auto gate = std::ranges::find(scenario.graph.nodes(), std::string{"Arrival Gate A4"},
        &airside::AirportNode::name);
    ASSERT_NE(detour, scenario.graph.nodes().end());
    ASSERT_NE(merge, scenario.graph.nodes().end());
    ASSERT_NE(gate, scenario.graph.nodes().end());
    const auto closed_edge = std::ranges::find_if(scenario.graph.edges(), [&](const auto& edge) {
        return edge.from == merge->id && edge.to == gate->id;
    });
    ASSERT_NE(closed_edge, scenario.graph.edges().end());
    const auto detour_id = detour->id;
    const auto closed_edge_id = closed_edge->id;
    airside::Simulation simulation(std::move(scenario), 42);
    for (std::size_t step = 0; step < 20000 && !simulation.finished(); ++step)
        (void)simulation.advance();
    ASSERT_TRUE(simulation.finished()) << "last time=" << simulation.snapshot().simulation_time.count();
    const auto result = simulation.result();
    EXPECT_EQ(result.metrics.surface_departed_aircraft, 3U);
    EXPECT_EQ(result.metrics.surface_arrived_aircraft, 2U);
    EXPECT_EQ(result.metrics.runway_operations_completed, 5U);
    EXPECT_EQ(result.metrics.surface_aircraft_aircraft_collisions, 0U);
    EXPECT_EQ(result.metrics.surface_aircraft_ground_collisions, 0U);
    EXPECT_EQ(result.metrics.surface_safe_failures, 0U);
    EXPECT_EQ(result.metrics.fleet_outstanding_reservations, 0U);
    EXPECT_EQ(std::ranges::count_if(result.events, [](const auto& event) {
        return event.type == airside::SimulationEventType::SurfaceReservationAcquired;
    }), std::ranges::count_if(result.events, [](const auto& event) {
        return event.type == airside::SimulationEventType::SurfaceReservationReleased;
    }));
    EXPECT_GT(result.metrics.maximum_runway_queue_depth, 0U);
    EXPECT_GT(result.metrics.runway_utilization, 0.0);
    EXPECT_LE(result.metrics.runway_utilization, 1.0);
    EXPECT_GE(result.metrics.minimum_aircraft_separation_m, 12.0);
    EXPECT_GE(result.metrics.minimum_aircraft_ground_separation_m, 8.0);
    EXPECT_EQ(std::ranges::count_if(result.events, [](const auto& event) {
        return event.type == airside::SimulationEventType::ArrivalAtGate;
    }), 2);
    const auto rerouted_arrival = std::ranges::find_if(result.events, [](const auto& event) {
        return event.type == airside::SimulationEventType::SurfaceTaxiRouteAssigned &&
            event.aircraft == airside::AircraftId{4};
    });
    ASSERT_NE(rerouted_arrival, result.events.end());
    ASSERT_TRUE(rerouted_arrival->route.has_value());
    EXPECT_NE(std::ranges::find(rerouted_arrival->route->nodes, detour_id), rerouted_arrival->route->nodes.end());
    EXPECT_EQ(std::ranges::find(rerouted_arrival->route->edges, closed_edge_id), rerouted_arrival->route->edges.end());
    EXPECT_GT(result.metrics.arrival_taxi_distance_m, 326.0);
    const auto departure_release = std::ranges::find_if(result.events, [](const auto& event) {
        return event.type == airside::SimulationEventType::RunwayReleased && event.aircraft == airside::AircraftId{1};
    });
    const auto arrival_request = std::ranges::find_if(result.events, [](const auto& event) {
        return event.type == airside::SimulationEventType::RunwayRequest && event.aircraft == airside::AircraftId{4};
    });
    const auto arrival_grant = std::ranges::find_if(result.events, [](const auto& event) {
        return event.type == airside::SimulationEventType::RunwayGrant && event.aircraft == airside::AircraftId{4};
    });
    ASSERT_NE(departure_release, result.events.end());
    ASSERT_NE(arrival_request, result.events.end());
    ASSERT_NE(arrival_grant, result.events.end());
    EXPECT_LT(arrival_request->timestamp, departure_release->timestamp);
    EXPECT_GE(arrival_grant->timestamp, departure_release->timestamp);
    const auto operation = std::ranges::find(result.metrics.aircraft, airside::AircraftId{4}, &airside::AircraftMetrics::id);
    ASSERT_NE(operation, result.metrics.aircraft.end());
    EXPECT_EQ(operation->operation_type, "arrival");
    EXPECT_GT(operation->arrival_taxi_distance_m, 0.0);
    EXPECT_TRUE(operation->gate_arrival_time.has_value());
}

TEST(SurfaceTrafficTest, ControlMixedTrafficUsesSharedRunwayAndReleasesEveryOperation) {
    airside::Simulation simulation(airside::load_scenario(scenario_file("mixed_runway_operations.yaml")), 42);
    const auto result = simulation.run();
    EXPECT_EQ(result.metrics.surface_arrived_aircraft, 2U);
    EXPECT_EQ(result.metrics.surface_departed_aircraft, 3U);
    EXPECT_EQ(result.metrics.runway_operations_completed, 5U);
    EXPECT_EQ(result.metrics.surface_total_aircraft, 5U);
    EXPECT_EQ(result.metrics.surface_aircraft_aircraft_collisions, 0U);
    EXPECT_EQ(result.metrics.surface_aircraft_ground_collisions, 0U);
    EXPECT_EQ(result.metrics.surface_safe_failures, 0U);
    EXPECT_EQ(result.metrics.runway_occupied_seconds, 1050);
    EXPECT_GT(result.metrics.arrival_runway_wait_seconds, 0);
    EXPECT_GT(result.metrics.arrival_taxi_distance_m, 0.0);
    EXPECT_GT(result.metrics.departure_taxi_distance_m, 0.0);
    std::vector<airside::AircraftId> granted;
    std::optional<airside::AircraftId> runway_owner;
    for (const auto& event : result.events) {
        if (event.type == airside::SimulationEventType::RunwayGrant) {
            ASSERT_TRUE(event.aircraft.has_value());
            EXPECT_FALSE(runway_owner.has_value());
            runway_owner = event.aircraft;
            granted.push_back(*event.aircraft);
        } else if (event.type == airside::SimulationEventType::RunwayOccupied) {
            EXPECT_TRUE(event.aircraft.has_value());
            EXPECT_EQ(runway_owner, event.aircraft);
        } else if (event.type == airside::SimulationEventType::RunwayReleased) {
            EXPECT_TRUE(event.aircraft.has_value());
            EXPECT_EQ(runway_owner, event.aircraft);
            runway_owner.reset();
        }
    }
    EXPECT_EQ(granted.size(), 5U);
    EXPECT_FALSE(runway_owner.has_value());
    const auto arrival_request = std::ranges::find_if(result.events, [](const auto& event) {
        return event.type == airside::SimulationEventType::RunwayRequest && event.aircraft == airside::AircraftId{4};
    });
    const auto departure_release = std::ranges::find_if(result.events, [](const auto& event) {
        return event.type == airside::SimulationEventType::RunwayReleased && event.aircraft == airside::AircraftId{1};
    });
    const auto arrival_grant = std::ranges::find_if(result.events, [](const auto& event) {
        return event.type == airside::SimulationEventType::RunwayGrant && event.aircraft == airside::AircraftId{4};
    });
    ASSERT_NE(arrival_request, result.events.end());
    ASSERT_NE(departure_release, result.events.end());
    ASSERT_NE(arrival_grant, result.events.end());
    EXPECT_LT(arrival_request->timestamp, departure_release->timestamp);
    EXPECT_LE(departure_release->timestamp, arrival_grant->timestamp);
    EXPECT_GT(result.metrics.arrival_runway_wait_seconds, 0);
}

TEST(SurfaceTrafficTest, SimultaneousArrivalRequestsUseStableAircraftIdTieBreakAndRepeatExactly) {
    const auto run = [] {
        auto scenario = airside::load_scenario(scenario_file("mixed_runway_operations.yaml"));
        const auto& second_arrival = scenario.aircraft[4];
        scenario.aircraft[4] = airside::Aircraft(second_arrival.id(), second_arrival.flight_number(), airside::SimTime{700},
            airside::SimTime{700}, second_arrival.gate(), second_arrival.gate_node(), second_arrival.tasks(),
            second_arrival.turnaround_id(), airside::SimTime{700}, second_arrival.operation_type(),
            second_arrival.arrival_exit_node());
        std::swap(scenario.aircraft[3], scenario.aircraft[4]);
        airside::Simulation simulation(std::move(scenario), 42);
        return simulation.run();
    };
    const auto first = run();
    const auto repeated = run();
    EXPECT_EQ(first.events, repeated.events);
    EXPECT_EQ(first.metrics.surface_arrived_aircraft, 2U);
    EXPECT_EQ(first.metrics.surface_departed_aircraft, 3U);
    const auto request_five = std::ranges::find_if(first.events, [](const auto& event) {
        return event.type == airside::SimulationEventType::RunwayRequest && event.aircraft == airside::AircraftId{5};
    });
    const auto request_four = std::ranges::find_if(first.events, [](const auto& event) {
        return event.type == airside::SimulationEventType::RunwayRequest && event.aircraft == airside::AircraftId{4};
    });
    const auto grant_four = std::ranges::find_if(first.events, [](const auto& event) {
        return event.type == airside::SimulationEventType::RunwayGrant && event.aircraft == airside::AircraftId{4};
    });
    const auto grant_five = std::ranges::find_if(first.events, [](const auto& event) {
        return event.type == airside::SimulationEventType::RunwayGrant && event.aircraft == airside::AircraftId{5};
    });
    ASSERT_NE(request_five, first.events.end());
    ASSERT_NE(request_four, first.events.end());
    ASSERT_NE(grant_four, first.events.end());
    ASSERT_NE(grant_five, first.events.end());
    EXPECT_LT(request_five->sequence, request_four->sequence);
    EXPECT_LT(grant_four->sequence, grant_five->sequence);
}

TEST(SurfaceTrafficTest, ArrivalWithNoReachableTaxiRouteFailsSafelyWithReason) {
    auto scenario = airside::load_scenario(scenario_file("mixed_runway_operations.yaml"));
    const auto exit = std::ranges::find(scenario.graph.nodes(), std::string{"Runway Exit A4"},
        &airside::AirportNode::name);
    ASSERT_NE(exit, scenario.graph.nodes().end());
    std::size_t disabled_edges = 0;
    for (const auto& edge : scenario.graph.edges()) {
        if (edge.from == exit->id || edge.to == exit->id) {
            scenario.graph.set_edge_available(edge.id, false);
            ++disabled_edges;
        }
    }
    ASSERT_GT(disabled_edges, 0U);
    airside::Simulation simulation(std::move(scenario), 42);
    const auto result = simulation.run();
    EXPECT_EQ(result.metrics.surface_safe_failures, 1U);
    EXPECT_EQ(result.metrics.surface_arrived_aircraft, 1U);
    const auto failure = std::ranges::find_if(result.events, [](const auto& event) {
        return event.type == airside::SimulationEventType::SurfaceSafeFailure &&
            event.aircraft == airside::AircraftId{4};
    });
    ASSERT_NE(failure, result.events.end());
    EXPECT_EQ(failure->detail, "no available taxi route from runway exit to assigned arrival gate");
    EXPECT_NE(airside::format_event(*failure).find("no available taxi route from runway exit to assigned arrival gate"),
        std::string::npos);
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
