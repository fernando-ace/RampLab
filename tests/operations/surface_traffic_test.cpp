#include "airside/operations/simulation.hpp"
#include "airside/operations/surface_safety.hpp"
#include "airside/scenario/scenario_loader.hpp"
#include "airside/experiment/executor.hpp"
#include "airside/experiment/experiment_loader.hpp"
#include "airside/experiment/case_generator.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <optional>
#include <ranges>
#include <stdexcept>

namespace {

std::filesystem::path scenario_file(const char* name) {
    return std::filesystem::path{AIRSIDE_SOURCE_DIR} / "scenarios" / name;
}

struct TaxiContentionHistory {
    std::optional<std::uint64_t> first_entry;
    std::optional<std::uint64_t> wait;
    std::optional<std::uint64_t> progression;
    std::optional<std::uint64_t> departure;
    std::optional<airside::SimTime> entry_time;
    std::optional<airside::SimTime> wait_time;
    std::optional<airside::SimTime> release_time;
};

TaxiContentionHistory taxi_contention_history(const airside::SimulationResult& result,
    airside::AircraftId aircraft, airside::EdgeId shared_edge) {
    TaxiContentionHistory history;
    for (const auto& event : result.events) {
        if (event.aircraft != aircraft) continue;
        if (event.type == airside::SimulationEventType::SurfaceReservationAcquired && event.edge == shared_edge) {
            if (!history.first_entry) {
                history.first_entry = event.sequence;
                history.entry_time = event.timestamp;
            }
        } else if (event.type == airside::SimulationEventType::SurfaceReservationReleased && event.edge == shared_edge && !history.progression) {
            history.progression = event.sequence;
            history.release_time = event.timestamp;
        } else if ((event.type == airside::SimulationEventType::SurfaceWaitingForTraffic
            || event.type == airside::SimulationEventType::SurfaceWaitingForPushback) && event.edge == shared_edge && !history.wait) {
            history.wait = event.sequence;
            history.wait_time = event.timestamp;
        } else if (event.type == airside::SimulationEventType::AircraftDeparted) {
            history.departure = event.sequence;
        }
    }
    return history;
}

airside::Scenario surface_ordering_scenario() {
    auto scenario = airside::load_scenario(scenario_file("surface_traffic.yaml"));
    // Stage the already-serviced synthetic A1 departure at South Junction.
    // A1 and A2 retain distinct gate IDs and distinct physical start nodes;
    // neither aircraft starts at a vehicle depot. No graph data changes.
    const auto stand = std::ranges::find(scenario.graph.nodes(),
        std::string{"South Junction"}, &airside::AirportNode::name);
    const auto a = std::ranges::find(scenario.aircraft, airside::AircraftId{1}, &airside::Aircraft::id);
    if (stand == scenario.graph.nodes().end() || a == scenario.aircraft.end())
        throw std::logic_error("missing synthetic ordering aircraft or stand");
    const auto gate = std::ranges::find(scenario.gates, a->gate(), &airside::Gate::id);
    if (gate == scenario.gates.end()) throw std::logic_error("missing synthetic A1 gate");
    gate->node = stand->id;
    a->reassign_gate(gate->id, stand->id);
    // A1 is already serviced in this focused departure fixture. Dispatching its
    // fuel and baggage vehicles to the same repurposed node would create an
    // unrelated GSE experiment. Retain its legitimate preparation timing task.
    auto preparation = a->task(airside::ServiceType::PushbackPreparation);
    preparation.prerequisites.clear();
    a->mutable_tasks() = {preparation};
    // Release this departure pair after mobile servicing has cleared the apron.
    // Earliest-start times avoid occupying the shared preparation resource during
    // the delay. Only A's preparation release changes between ordering variants.
    a->mutable_tasks().front().duration = airside::SimTime{10};
    a->mutable_tasks().front().earliest_start = airside::SimTime{1100};
    const auto b = std::ranges::find(scenario.aircraft, airside::AircraftId{2}, &airside::Aircraft::id);
    if (b == scenario.aircraft.end()) throw std::logic_error("missing AX202");
    for (auto& task : b->mutable_tasks()) {
        if (task.type == airside::ServiceType::PushbackPreparation) {
            task.duration = airside::SimTime{10};
            task.earliest_start = airside::SimTime{1000};
        }
    }
    return scenario;
}

airside::SimulationResult run_surface_ordering_variant(airside::SimTime a_preparation_offset) {
    auto scenario = surface_ordering_scenario();
    const auto a = std::ranges::find(scenario.aircraft, airside::AircraftId{1}, &airside::Aircraft::id);
    a->mutable_tasks().front().earliest_start += a_preparation_offset;
    airside::Simulation simulation(std::move(scenario), 42);
    std::size_t steps = 0;
    while (!simulation.finished() && steps < 20000) {
        (void)simulation.advance();
        ++steps;
    }
    if (!simulation.finished()) throw std::runtime_error("ordering fixture did not terminate");
    return simulation.result();
}

airside::EdgeId north_south_taxi_edge(const airside::AirportGraph& graph) {
    const auto north = std::ranges::find_if(graph.nodes(), [](const auto& node) { return node.name == "North Junction"; });
    const auto south = std::ranges::find_if(graph.nodes(), [](const auto& node) { return node.name == "South Junction"; });
    if (north == graph.nodes().end() || south == graph.nodes().end()) return {};
    const auto edge = std::ranges::find_if(graph.edges(), [&](const auto& candidate) {
        return candidate.from == north->id && candidate.to == south->id;
    });
    return edge == graph.edges().end() ? airside::EdgeId{} : edge->id;
}

airside::Scenario synthetic_stand_closure(airside::RecoveryMode mode) {
    airside::Scenario scenario;
    scenario.name = "synthetic_stand_closure";
    scenario.turnaround_orchestration = true;
    const airside::NodeId runway{1}, exit{2}, gate_a_node{3}, gate_b_node{4}, handoff{5};
    scenario.graph.add_node({runway, "runway", {0.0, 0.0}});
    scenario.graph.add_node({exit, "exit", {10.0, 0.0}});
    scenario.graph.add_node({gate_a_node, "stand-a", {20.0, 0.0}});
    scenario.graph.add_node({gate_b_node, "stand-b", {20.0, 10.0}});
    scenario.graph.add_node({handoff, "handoff", {30.0, 0.0}});
    scenario.graph.add_edge({airside::EdgeId{1}, exit, gate_a_node, 10.0, airside::SimTime{1}, true, false});
    scenario.graph.add_edge({airside::EdgeId{2}, exit, gate_b_node, 15.0, airside::SimTime{1}, true, false});
    scenario.graph.add_edge({airside::EdgeId{3}, gate_a_node, handoff, 10.0, airside::SimTime{1}, true, false});
    scenario.graph.add_edge({airside::EdgeId{4}, gate_b_node, handoff, 15.0, airside::SimTime{1}, true, false});
    scenario.gates.emplace_back(airside::GateId{1}, "A", gate_a_node);
    scenario.gates.emplace_back(airside::GateId{2}, "B", gate_b_node);
    airside::ServiceTask task{airside::TaskId{1}, airside::ServiceType::Deboarding};
    task.duration = airside::SimTime{1};
    scenario.abstract_resource_capacity.emplace(airside::ServiceType::Deboarding, 1U);
    scenario.aircraft.emplace_back(airside::AircraftId{1}, "SYN001", airside::SimTime{0},
        airside::SimTime{100}, airside::GateId{1}, gate_a_node,
        std::vector<airside::ServiceTask>{task}, "SYN001", airside::SimTime{80},
        airside::AircraftOperationType::ArrivalTurnaround, exit);
    scenario.surface_operations = airside::SurfaceOperationsConfig{
        handoff, runway, exit, airside::SimTime{1}, airside::SimTime{2}, airside::SimTime{1},
        100.0, 100.0};
    scenario.gate_events.push_back({airside::SimTime{1}, airside::GateId{1}, false});
    scenario.recovery_mode = mode;
    return scenario;
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

void validate_surface_ordering_run(const airside::SimulationResult& result, airside::EdgeId edge) {
    // Prove actual assigned routes before any order assertions.
    for (const auto id : {airside::AircraftId{1}, airside::AircraftId{2}}) {
        EXPECT_TRUE(std::ranges::any_of(result.events, [&](const auto& event) {
            return event.aircraft == id && event.type == airside::SimulationEventType::SurfaceTaxiRouteAssigned
                && event.route && std::ranges::find(event.route->edges, edge) != event.route->edges.end();
        })) << "aircraft " << id.value() << " did not receive the shared-edge route";
    }
    std::optional<airside::AircraftId> owner;
    for (const auto& event : result.events) {
        if (event.edge != edge) continue;
        if (event.type == airside::SimulationEventType::SurfaceReservationAcquired) {
            EXPECT_FALSE(owner);
            owner = event.aircraft;
        } else if (event.type == airside::SimulationEventType::SurfaceReservationReleased) {
            EXPECT_EQ(owner, event.aircraft);
            owner.reset();
        }
    }
    EXPECT_FALSE(owner);
    EXPECT_EQ(result.metrics.surface_departed_aircraft, 3U);
    EXPECT_EQ(result.metrics.surface_safe_failures, 0U);
    EXPECT_EQ(result.metrics.fleet_collisions, 0U);
    EXPECT_EQ(result.metrics.surface_aircraft_aircraft_collisions, 0U);
    EXPECT_EQ(result.metrics.surface_aircraft_ground_collisions, 0U);
    EXPECT_EQ(result.metrics.fleet_outstanding_reservations, 0U);
    EXPECT_EQ(std::ranges::count(result.events, airside::SimulationEventType::SurfaceReservationAcquired,
        &airside::SimulationEventRecord::type),
        std::ranges::count(result.events, airside::SimulationEventType::SurfaceReservationReleased,
            &airside::SimulationEventRecord::type));
    for (const auto id : {airside::AircraftId{1}, airside::AircraftId{2}}) {
        bool printed_wait = false;
        for (const auto& event : result.events) {
            if (event.aircraft != id) continue;
            const bool wait = event.type == airside::SimulationEventType::SurfaceWaitingForTraffic
                || event.type == airside::SimulationEventType::SurfaceWaitingForPushback;
            if ((event.edge == edge && (event.type == airside::SimulationEventType::SurfaceReservationAcquired
                || event.type == airside::SimulationEventType::SurfaceReservationReleased || (wait && !printed_wait)))
                || event.type == airside::SimulationEventType::AircraftDeparted) {
                std::cout << "ordering aircraft=" << id.value() << " t=" << event.timestamp.count()
                    << " event=" << airside::to_string(event.type) << " detail=" << event.detail << '\n';
                if (wait) printed_wait = true;
            }
        }
    }
}

TEST(SurfaceTrafficTest, Goal49NorthSouthTaxiNeutralContention) {
    const auto scenario = surface_ordering_scenario();
    const auto edge = north_south_taxi_edge(scenario.graph);
    ASSERT_NE(edge, airside::EdgeId{});
    // Route validity gate uses the same aircraft routing API as the simulation.
    for (const auto id : {airside::AircraftId{1}, airside::AircraftId{2}}) {
        const auto aircraft = std::ranges::find(scenario.aircraft, id, &airside::Aircraft::id);
        ASSERT_NE(aircraft, scenario.aircraft.end());
        const auto route = airside::find_route(scenario.graph, aircraft->gate_node(),
            scenario.surface_operations->departure_handoff, airside::MovementClass::Aircraft);
        ASSERT_TRUE(route);
        ASSERT_NE(std::ranges::find(route->edges, edge), route->edges.end());
    }
    const auto result = run_surface_ordering_variant(airside::SimTime{0});
    validate_surface_ordering_run(result, edge);
    const auto a = taxi_contention_history(result, airside::AircraftId{1}, edge);
    const auto b = taxi_contention_history(result, airside::AircraftId{2}, edge);
    EXPECT_TRUE(a.first_entry);
    EXPECT_TRUE(b.first_entry);
    EXPECT_TRUE(a.progression);
    EXPECT_TRUE(b.progression);
    EXPECT_TRUE(a.departure);
    EXPECT_TRUE(b.departure);
}

TEST(SurfaceTrafficTest, Goal49NorthSouthTaxiAFirstOrdering) {
    const auto scenario = airside::load_scenario(scenario_file("surface_traffic.yaml"));
    const auto edge = north_south_taxi_edge(scenario.graph);
    ASSERT_NE(edge, airside::EdgeId{});
    constexpr airside::AircraftId aircraft_a{1}; // AX101
    constexpr airside::AircraftId aircraft_b{2}; // AX202
    const auto result = run_surface_ordering_variant(airside::SimTime{-100});
    validate_surface_ordering_run(result, edge);
    const auto a = taxi_contention_history(result, aircraft_a, edge);
    const auto b = taxi_contention_history(result, aircraft_b, edge);
    ASSERT_TRUE(a.first_entry);
    ASSERT_TRUE(b.first_entry);
    EXPECT_LT(*a.first_entry, *b.first_entry);
    EXPECT_LT(*a.entry_time, *b.entry_time);
    ASSERT_TRUE(b.wait);
    EXPECT_LT(*a.first_entry, *b.wait);
    ASSERT_TRUE(a.progression);
    EXPECT_LT(*b.wait, *a.progression);
    EXPECT_LT(*b.wait_time, *a.release_time);
    EXPECT_LT(*b.wait, *b.first_entry);
    EXPECT_LE(*a.release_time, *b.entry_time);
    ASSERT_TRUE(b.progression);
    EXPECT_LT(*b.wait, *b.progression);
    EXPECT_TRUE(a.departure);
    EXPECT_TRUE(b.departure);
}

TEST(SurfaceTrafficTest, Goal49NorthSouthTaxiBFirstOrdering) {
    const auto scenario = airside::load_scenario(scenario_file("surface_traffic.yaml"));
    const auto edge = north_south_taxi_edge(scenario.graph);
    ASSERT_NE(edge, airside::EdgeId{});
    constexpr airside::AircraftId aircraft_a{1}; // AX101
    constexpr airside::AircraftId aircraft_b{2}; // AX202
    const auto result = run_surface_ordering_variant(airside::SimTime{200});
    validate_surface_ordering_run(result, edge);
    const auto a = taxi_contention_history(result, aircraft_a, edge);
    const auto b = taxi_contention_history(result, aircraft_b, edge);
    ASSERT_TRUE(a.first_entry);
    ASSERT_TRUE(b.first_entry);
    EXPECT_LT(*b.first_entry, *a.first_entry);
    EXPECT_LT(*b.entry_time, *a.entry_time);
    ASSERT_TRUE(a.wait);
    EXPECT_LT(*b.first_entry, *a.wait);
    ASSERT_TRUE(b.progression);
    EXPECT_LT(*a.wait, *b.progression);
    EXPECT_LT(*a.wait_time, *b.release_time);
    EXPECT_LT(*a.wait, *a.first_entry);
    EXPECT_LE(*b.release_time, *a.entry_time);
    ASSERT_TRUE(a.progression);
    EXPECT_LT(*a.wait, *a.progression);
    EXPECT_TRUE(a.departure);
    EXPECT_TRUE(b.departure);
}

TEST(SurfaceTrafficTest, ControlledHoldGatesPushbackUntilExactReleaseWhileOtherAircraftMoves) {
    auto scenario = synthetic_stand_closure(airside::RecoveryMode::Integrated);
    scenario.gate_events.clear();
    const airside::NodeId exit{2}, gate_b_node{4};
    scenario.aircraft.emplace_back(airside::AircraftId{2}, "SYN002", airside::SimTime{0},
        airside::SimTime{100}, airside::GateId{2}, gate_b_node, std::vector<airside::ServiceTask>{},
        "", std::nullopt, airside::AircraftOperationType::ArrivalOnly, exit);
    scenario.gates.emplace_back(airside::GateId{2}, "B", gate_b_node);
    scenario.recovery_mode = airside::RecoveryMode::Integrated;
    airside::Simulation simulation(std::move(scenario), 42);
    const airside::RecoveryAction hold{airside::RecoveryActionKind::ControlledHold,
        "1", "60", "synthetic surface contention"};
    EXPECT_TRUE(simulation.validate_recovery_action(hold).accepted);
    EXPECT_TRUE(simulation.apply_recovery_action(hold).accepted);
    ASSERT_TRUE(simulation.active_recovery_holds().contains(airside::AircraftId{1}));
    const auto parent_time = simulation.current_time();
    const auto parent_history = simulation.event_history();
    const auto replay = simulation.replay_to_recovery_boundary();
    EXPECT_TRUE(replay.equivalent) << replay.reason;
    ASSERT_NE(replay.simulation, nullptr);
    ASSERT_TRUE(replay.simulation->active_recovery_holds().contains(airside::AircraftId{1}));
    while (!replay.simulation->active_recovery_holds().empty() && !replay.simulation->finished())
        (void)replay.simulation->advance();
    EXPECT_TRUE(replay.simulation->active_recovery_holds().empty());
    EXPECT_TRUE(std::ranges::any_of(replay.simulation->event_history(), [](const auto& event) {
        return event.type == airside::SimulationEventType::RecoveryHoldReleased &&
            event.aircraft == airside::AircraftId{1} && event.timestamp == airside::SimTime{60};
    }));
    EXPECT_EQ(simulation.current_time(), parent_time);
    EXPECT_EQ(simulation.event_history(), parent_history);
    EXPECT_EQ(simulation.validate_recovery_action(hold).reason, "recovery_hold_already_active");
    const auto events = [&] { return simulation.event_history(); };
    while (simulation.current_time() < airside::SimTime{60} && !simulation.finished()) (void)simulation.advance();
    EXPECT_EQ(simulation.current_time(), airside::SimTime{60});
    EXPECT_TRUE(simulation.active_recovery_holds().empty());
    const auto history_at_release = events();
    const auto release = std::ranges::find_if(history_at_release, [](const auto& event) {
        return event.type == airside::SimulationEventType::RecoveryHoldReleased &&
            event.aircraft == airside::AircraftId{1};
    });
    ASSERT_NE(release, history_at_release.end());
    EXPECT_EQ(release->timestamp, airside::SimTime{60});
    EXPECT_TRUE(std::ranges::any_of(history_at_release, [](const auto& event) {
        return event.type == airside::SimulationEventType::ArrivalTaxiInStarted &&
            event.aircraft == airside::AircraftId{2} && event.timestamp < airside::SimTime{60};
    }));
    ASSERT_FALSE(std::ranges::any_of(history_at_release, [](const auto& event) {
        return event.type == airside::SimulationEventType::SurfacePushbackStarted &&
            event.aircraft == airside::AircraftId{1};
    }));
    while (!simulation.finished() && !std::ranges::any_of(simulation.event_history(), [](const auto& event) {
        return event.type == airside::SimulationEventType::SurfacePushbackStarted &&
            event.aircraft == airside::AircraftId{1};
    })) (void)simulation.advance();
    const auto pushback = std::ranges::find_if(simulation.event_history(), [](const auto& event) {
        return event.type == airside::SimulationEventType::SurfacePushbackStarted &&
            event.aircraft == airside::AircraftId{1};
    });
    ASSERT_NE(pushback, simulation.event_history().end());
    EXPECT_GE(pushback->timestamp, airside::SimTime{60});
    EXPECT_TRUE(simulation.active_recovery_holds().empty());
}

TEST(SurfaceTrafficTest, ControlledHoldRejectsMalformedAndOutOfBoundsDurations) {
    auto scenario = synthetic_stand_closure(airside::RecoveryMode::Reactive);
    scenario.gate_events.clear();
    airside::Simulation simulation(std::move(scenario), 42);
    const auto validate = [&](std::string target) {
        return simulation.validate_recovery_action({airside::RecoveryActionKind::ControlledHold,
            "1", std::move(target), "test"});
    };
    EXPECT_EQ(validate("bad").reason, "invalid_hold_duration");
    EXPECT_EQ(validate("-1").reason, "hold_duration_must_be_positive");
    EXPECT_EQ(validate("0").reason, "hold_duration_must_be_positive");
    EXPECT_TRUE(validate("1").accepted);
    EXPECT_TRUE(validate("600").accepted);
    EXPECT_EQ(validate("601").reason, "hold_duration_exceeds_600_seconds");
    EXPECT_EQ(simulation.validate_recovery_action({airside::RecoveryActionKind::ControlledHold,
        "999", "60", "test"}).reason, "aircraft_not_found");

    auto failed_scenario = synthetic_stand_closure(airside::RecoveryMode::Reactive);
    failed_scenario.gate_events.clear();
    failed_scenario.aircraft.front().fail_turnaround("synthetic failed state");
    airside::Simulation failed(std::move(failed_scenario), 42);
    EXPECT_EQ(failed.validate_recovery_action({airside::RecoveryActionKind::ControlledHold,
        "1", "60", "test"}).reason, "aircraft_failed");

    auto moving_scenario = synthetic_stand_closure(airside::RecoveryMode::Reactive);
    moving_scenario.gate_events.clear();
    airside::Simulation moving(std::move(moving_scenario), 42);
    auto has_pushback = [&] {
        return std::ranges::any_of(moving.event_history(), [](const auto& event) {
            return event.type == airside::SimulationEventType::SurfacePushbackStarted &&
                event.aircraft == airside::AircraftId{1};
        });
    };
    while (!moving.finished() && !has_pushback()) (void)moving.advance();
    EXPECT_TRUE(has_pushback());
    EXPECT_EQ(moving.validate_recovery_action({airside::RecoveryActionKind::ControlledHold,
        "1", "60", "test"}).reason, "aircraft_in_unsupported_surface_phase");
}

TEST(SurfaceTrafficTest, ControlledHoldRejectsEveryUnsupportedOperationalPhase) {
    const auto rejects_in_phase = [](std::string_view phase_name, std::string_view expected_state) {
        airside::Simulation simulation(airside::load_scenario(scenario_file("surface_traffic.yaml")), 42);
        for (std::size_t step = 0; step < 20000 && !simulation.finished(); ++step) {
            const auto snapshot = simulation.snapshot();
            const auto aircraft = std::ranges::find_if(snapshot.aircraft, [expected_state](const auto& item) {
                return item.surface_state == expected_state;
            });
            if (aircraft != snapshot.aircraft.end()) {
                const auto result = simulation.validate_recovery_action({
                    airside::RecoveryActionKind::ControlledHold, std::to_string(aircraft->id.value()), "60", "phase test"});
                EXPECT_EQ(result.reason, "aircraft_in_unsupported_surface_phase") << phase_name;
                return true;
            }
            (void)simulation.advance();
        }
        ADD_FAILURE() << "surface_traffic.yaml did not expose phase " << phase_name;
        return false;
    };
    EXPECT_TRUE(rejects_in_phase("Pushback", "Pushback"));
    EXPECT_TRUE(rejects_in_phase("Taxiing", "Taxiing"));
    EXPECT_TRUE(rejects_in_phase("WaitingForTraffic", "WaitingForTraffic"));
    EXPECT_TRUE(rejects_in_phase("WaitingForRunway", "WaitingForRunway"));
    EXPECT_TRUE(rejects_in_phase("Runway", "RunwayOccupied"));

    airside::Simulation departed(airside::load_scenario(scenario_file("surface_traffic.yaml")), 42);
    const auto ids = departed.snapshot().aircraft;
    ASSERT_FALSE(ids.empty());
    const auto target = ids.front().id;
    (void)departed.run();
    EXPECT_EQ(departed.validate_recovery_action({airside::RecoveryActionKind::ControlledHold,
        std::to_string(target.value()), "60", "departed test"}).reason, "aircraft_departed");
}

TEST(SurfaceTrafficTest, MovementEventsCarryAuthoritativePoseAndStableEntityIdentity) {
    airside::Simulation simulation(airside::load_scenario(scenario_file("surface_traffic.yaml")), 42);
    const auto result = simulation.run();
    const auto movement = std::ranges::find_if(result.events, [](const auto& event) {
        return event.type == airside::SimulationEventType::SurfaceReservationAcquired && event.aircraft;
    });
    ASSERT_NE(movement, result.events.end());
    ASSERT_TRUE(movement->position_m.has_value());
    ASSERT_TRUE(movement->heading_rad.has_value());
    EXPECT_TRUE(std::isfinite(movement->position_m->x_m));
    EXPECT_TRUE(std::isfinite(movement->position_m->y_m));
    EXPECT_TRUE(std::isfinite(*movement->heading_rad));
}

TEST(SurfaceTrafficTest, RoutesAroundStationaryGroundVehicleBeforeEnteringEdge) {
    const auto make_scenario = [] {
        airside::Scenario scenario;
        scenario.name = "stationary_gse_route_blocker";
        scenario.turnaround_orchestration = true;
        const airside::NodeId runway{1}, exit{2}, gate_node{3}, handoff{4}, depot{5}, detour{6};
        scenario.graph.add_node({runway, "runway", {0.0, -500.0}});
        scenario.graph.add_node({exit, "exit", {0.0, 0.0}});
        scenario.graph.add_node({gate_node, "gate", {100.0, 0.0}});
        scenario.graph.add_node({handoff, "handoff", {200.0, 0.0}});
        scenario.graph.add_node({depot, "depot", {50.0, 20.0}});
        scenario.graph.add_node({detour, "detour", {0.0, 150.0}});
        airside::RoadEdge direct{airside::EdgeId{1}, exit, gate_node, 100.0,
            airside::SimTime{20}, true, false};
        direct.centerline = {{0.0, 0.0}, {50.0, 0.0}, {100.0, 0.0}};
        scenario.graph.add_edge(std::move(direct));
        scenario.graph.add_edge({airside::EdgeId{2}, exit, detour, 150.0, airside::SimTime{30}, true, false});
        scenario.graph.add_edge({airside::EdgeId{3}, detour, gate_node, 180.0, airside::SimTime{36}, true, false});
        scenario.gates.emplace_back(airside::GateId{1}, "A1", gate_node);
        scenario.vehicles.emplace_back(airside::VehicleId{1}, "Parked-GSE",
            airside::ServiceType::Fueling, depot);
        scenario.aircraft.emplace_back(airside::AircraftId{1}, "INBOUND001", airside::SimTime{0},
            airside::SimTime{1000}, airside::GateId{1}, gate_node, std::vector<airside::ServiceTask>{},
            "", std::nullopt, airside::AircraftOperationType::ArrivalOnly, exit);
        scenario.surface_operations = airside::SurfaceOperationsConfig{
            handoff, runway, exit, airside::SimTime{1}, airside::SimTime{2}, airside::SimTime{1}, 5.0, 30.0};
        scenario.surface_operations->aircraft_routing_policy = airside::agents::AutonomyMode::Adaptive;
        return scenario;
    };

    airside::Simulation simulation{make_scenario(), 42};
    std::size_t steps = 0;
    constexpr std::size_t max_steps = 500;
    while (!simulation.finished() && steps < max_steps) {
        (void)simulation.advance();
        ++steps;
    }
    if (!simulation.finished()) {
        const auto pending = simulation.snapshot();
        for (const auto& item : pending.aircraft) {
            std::cerr << "blocked aircraft " << item.flight_number << " state=" << item.surface_state
                      << " node=" << (item.surface_node ? std::to_string(item.surface_node->value()) : "none")
                      << " reason=" << item.surface_wait_reason
                      << " wait_s=" << item.surface_wait_duration.count()
                      << " pos=" << (item.surface_position_m ? std::to_string(item.surface_position_m->x_m) + "," +
                          std::to_string(item.surface_position_m->y_m) : "none") << " route=";
            for (const auto node : item.surface_route) std::cerr << node.value() << ',';
            std::cerr << '\n';
        }
        for (const auto& event : simulation.event_history()) {
            if (event.type == airside::SimulationEventType::SurfaceWaitingForTraffic ||
                event.type == airside::SimulationEventType::SurfaceReservationAcquired ||
                event.type == airside::SimulationEventType::SurfaceReservationReleased) {
                std::cerr << "diagnostic event t=" << event.timestamp.count() << " type="
                          << airside::to_string(event.type) << " aircraft="
                          << (event.aircraft ? std::to_string(event.aircraft->value()) : "none") << " edge="
                          << (event.edge ? std::to_string(event.edge->value()) : "none") << " vehicle="
                          << (event.vehicle ? std::to_string(event.vehicle->value()) : "none")
                          << " detail=" << event.detail << '\n';
            }
        }
    }
    ASSERT_TRUE(simulation.finished()) << "stationary ground vehicle left aircraft indefinitely blocked after "
        << steps << " events at simulated second " << simulation.current_time().count();

    const auto result = simulation.result();
    for (int repeat = 0; repeat < 2; ++repeat) {
        const auto repeated = airside::Simulation{make_scenario(), 42}.run();
        EXPECT_EQ(repeated.events, result.events) << "serial replay " << (repeat + 2) << " diverged";
        EXPECT_EQ(repeated.metrics.surface_aircraft_ground_collisions,
            result.metrics.surface_aircraft_ground_collisions);
    }
    EXPECT_EQ(result.metrics.surface_arrived_aircraft, 1U);
    const auto route = std::ranges::find_if(result.events, [](const auto& event) {
        return event.type == airside::SimulationEventType::SurfaceTaxiRouteAssigned && event.route.has_value();
    });
    ASSERT_NE(route, result.events.end());
    EXPECT_EQ(route->route->nodes,
        (std::vector<airside::NodeId>{airside::NodeId{2}, airside::NodeId{6}, airside::NodeId{3}}));
    EXPECT_EQ(result.metrics.surface_aircraft_ground_collisions, 0U);
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

TEST(SurfaceTrafficTest, IntegratedStandClosureSelectsAlternateAndShadowOnlyRecommends) {
    airside::Simulation integrated{synthetic_stand_closure(airside::RecoveryMode::Integrated), 42};
    const auto result = integrated.run();
    ASSERT_FALSE(integrated.recovery_epochs().empty());
    const auto& epoch = integrated.recovery_epochs().front();
    EXPECT_EQ(epoch.trigger, "stand_closure");
    const auto baseline = std::ranges::find(epoch.evaluations, "current_plan",
        [](const auto& evaluation) { return evaluation.candidate.id; });
    ASSERT_NE(baseline, epoch.evaluations.end());
    EXPECT_TRUE(baseline->valid);
    EXPECT_EQ(baseline->outcome.timeouts, 1U);
    const auto stand_prediction = std::ranges::find(epoch.evaluations, "stand:1:2",
        [](const auto& evaluation) { return evaluation.candidate.id; });
    ASSERT_NE(stand_prediction, epoch.evaluations.end());
    EXPECT_TRUE(stand_prediction->valid);
    EXPECT_GT(stand_prediction->private_events, 0U);
    ASSERT_TRUE(std::ranges::any_of(epoch.candidates, [](const auto& candidate) {
        return candidate.id == "stand:1:2" && candidate.actions.front().kind ==
            airside::RecoveryActionKind::ReassignStand;
    }));
    ASSERT_TRUE(std::ranges::any_of(epoch.evaluations, [](const auto& evaluation) {
        return evaluation.candidate.id == "stand:1:2" && evaluation.valid;
    }));
    ASSERT_EQ(epoch.selected_candidate_id, std::optional<std::string>{"stand:1:2"});
    EXPECT_EQ(epoch.application_result, "accepted");
    EXPECT_EQ(result.aircraft.front().gate(), airside::GateId{2});
    EXPECT_EQ(result.aircraft.front().state(), airside::AircraftState::Departed);
    EXPECT_TRUE(std::ranges::any_of(result.events, [](const auto& event) {
        return event.type == airside::SimulationEventType::ArrivalAtGate && event.gate == airside::GateId{2};
    }));
    const auto replay = integrated.replay_to_recovery_boundary();
    EXPECT_TRUE(replay.equivalent) << replay.reason;

    airside::Simulation reactive{synthetic_stand_closure(airside::RecoveryMode::Reactive), 42};
    airside::Simulation shadow{synthetic_stand_closure(airside::RecoveryMode::Shadow), 42};
    for (std::size_t step = 0; step < 20 && shadow.recovery_epochs().empty(); ++step) {
        ASSERT_TRUE(reactive.advance());
        ASSERT_TRUE(shadow.advance());
    }
    ASSERT_FALSE(shadow.recovery_epochs().empty());
    const auto& shadow_epoch = shadow.recovery_epochs().front();
    EXPECT_TRUE(std::ranges::any_of(shadow_epoch.evaluations, [](const auto& evaluation) {
        return evaluation.candidate.id == "stand:1:2" && evaluation.valid;
    }));
    EXPECT_EQ(shadow_epoch.selected_candidate_id, std::optional<std::string>{"stand:1:2"});
    EXPECT_TRUE(shadow_epoch.application_result.empty());
    for (std::size_t step = 0; step < 20; ++step) {
        ASSERT_TRUE(reactive.advance());
        ASSERT_TRUE(shadow.advance());
    }
    EXPECT_EQ(reactive.event_history(), shadow.event_history());
    EXPECT_EQ(reactive.snapshot(), shadow.snapshot());
}

TEST(SurfaceTrafficTest, StationaryDepartureQueueSkipsToNextScheduledOperation) {
    airside::Scenario scenario;
    scenario.name = "stationary_departure_queue";
    scenario.turnaround_orchestration = true;
    const airside::NodeId runway{1}, exit{2}, gate_node{3}, handoff{4};
    scenario.graph.add_node({runway, "runway", {300.0, 0.0}});
    scenario.graph.add_node({exit, "exit", {100.0, 0.0}});
    scenario.graph.add_node({gate_node, "gate", {200.0, 0.0}});
    scenario.graph.add_node({handoff, "handoff", {400.0, 0.0}});
    const airside::NodeId depot{5};
    scenario.graph.add_node({depot, "depot", {1000.0, 1000.0}});
    scenario.graph.add_edge({airside::EdgeId{1}, exit, gate_node, 100.0, airside::SimTime{1}, true, false});
    scenario.graph.add_edge({airside::EdgeId{2}, gate_node, handoff, 100.0, airside::SimTime{1}, true, false});
    const airside::GateId stand{1};
    scenario.gates.emplace_back(stand, "A1", gate_node);
    scenario.vehicles.emplace_back(airside::VehicleId{1}, "Fuel-1", airside::ServiceType::Fueling, depot);
    scenario.vehicles.emplace_back(airside::VehicleId{2}, "Baggage-1", airside::ServiceType::Baggage, depot);
    airside::ServiceTask deboard{airside::TaskId{1}, airside::ServiceType::Deboarding};
    deboard.duration = airside::SimTime{1};
    airside::ServiceTask prepare{airside::TaskId{2}, airside::ServiceType::PushbackPreparation};
    prepare.duration = airside::SimTime{1};
    prepare.prerequisites.push_back(deboard.id);
    scenario.aircraft.emplace_back(airside::AircraftId{1}, "LONG001", airside::SimTime{0},
        airside::SimTime{10000}, stand, gate_node, std::vector<airside::ServiceTask>{deboard, prepare},
        "TO-LONG001", airside::SimTime{10000}, airside::AircraftOperationType::ArrivalTurnaround, exit);
    scenario.service_durations.emplace(airside::ServiceType::Fueling, airside::SimTime{1});
    scenario.service_durations.emplace(airside::ServiceType::Baggage, airside::SimTime{1});
    scenario.abstract_resource_capacity.emplace(airside::ServiceType::Deboarding, 1U);
    scenario.abstract_resource_capacity.emplace(airside::ServiceType::PushbackPreparation, 1U);
    scenario.surface_operations = airside::SurfaceOperationsConfig{
        handoff, runway, exit, airside::SimTime{1}, airside::SimTime{2}, airside::SimTime{2}, 100.0, 100.0};

    airside::Simulation simulation{std::move(scenario), 42};
    std::size_t steps = 0;
    while (!simulation.finished() && steps < 100U) {
        (void)simulation.advance();
        ++steps;
    }
    ASSERT_TRUE(simulation.finished()) << "stationary queue did not jump to its next scheduled event";
    EXPECT_LT(steps, 100U);
    EXPECT_EQ(simulation.result().metrics.surface_departed_aircraft, 1U);
    EXPECT_GE(simulation.current_time().count(), 10000);
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

TEST(SurfaceTrafficTest, AdaptiveAgentsReplanDeterministicallyAndCompleteAfterScheduledClosure) {
    auto first_scenario = airside::load_scenario(scenario_file("surface_traffic_disrupted.yaml"));
    auto second_scenario = airside::load_scenario(scenario_file("surface_traffic_disrupted.yaml"));
    first_scenario.surface_operations->aircraft_routing_policy = airside::agents::AutonomyMode::Adaptive;
    second_scenario.surface_operations->aircraft_routing_policy = airside::agents::AutonomyMode::Adaptive;
    airside::Simulation first{std::move(first_scenario), 42};
    airside::Simulation second{std::move(second_scenario), 42};
    const auto first_result = first.run();
    const auto second_result = second.run();

    ASSERT_FALSE(first_result.agent_decisions.empty());
    EXPECT_EQ(first_result.agent_decisions, second_result.agent_decisions);
    EXPECT_EQ(first_result.metrics.agent_decisions, first_result.agent_decisions.size());
    EXPECT_GE(first_result.metrics.agent_replans, 1U);
    EXPECT_EQ(first_result.metrics.agent_objectives_failed, 0U);
    EXPECT_EQ(first_result.metrics.surface_safe_failures, 0U);
    EXPECT_EQ(first_result.metrics.surface_aircraft_aircraft_collisions, 0U);
    EXPECT_EQ(first_result.metrics.surface_aircraft_ground_collisions, 0U);
    EXPECT_TRUE(std::ranges::any_of(first_result.agent_decisions, [](const auto& decision) {
        return decision.reason == airside::agents::ReasonCode::RouteClosed &&
            decision.action == airside::agents::ActionType::PlanRoute && decision.selected_route.has_value();
    }));
    EXPECT_EQ(first_result.metrics.surface_departed_aircraft, 3U);
}

TEST(SurfaceTrafficTest, CoordinatedPolicyHandlesClosureWithoutBypassingSafetyOrLosingDeterminism) {
    auto first_scenario = airside::load_scenario(scenario_file("surface_traffic_disrupted.yaml"));
    auto second_scenario = airside::load_scenario(scenario_file("surface_traffic_disrupted.yaml"));
    first_scenario.surface_operations->aircraft_routing_policy = airside::agents::AutonomyMode::CoordinatedAdaptive;
    second_scenario.surface_operations->aircraft_routing_policy = airside::agents::AutonomyMode::CoordinatedAdaptive;
    const auto first = airside::Simulation{std::move(first_scenario), 42}.run();
    const auto second = airside::Simulation{std::move(second_scenario), 42}.run();

    EXPECT_EQ(first.agent_decisions, second.agent_decisions);
    EXPECT_EQ(first.events, second.events);
    EXPECT_EQ(first.metrics.agent_objectives_failed, 0U);
    EXPECT_EQ(first.metrics.surface_safe_failures, 0U);
    EXPECT_EQ(first.metrics.surface_aircraft_aircraft_collisions, 0U);
    EXPECT_EQ(first.metrics.surface_aircraft_ground_collisions, 0U);
    EXPECT_EQ(first.metrics.surface_departed_aircraft, 3U);
    EXPECT_TRUE(std::ranges::any_of(first.agent_decisions, [](const auto& decision) {
        return decision.policy_id == "coordinated-adaptive-routing-v1" &&
            decision.reason == airside::agents::ReasonCode::RouteClosed && decision.selected_route.has_value() &&
            !decision.safety_constraints.empty();
    }));
}

TEST(SurfaceTrafficTest, RollingHorizonPolicyProducesDeterministicSafeForecastTrace) {
    auto first_scenario = airside::load_scenario(scenario_file("goal37_coordinated_route_choice.yaml"));
    auto second_scenario = airside::load_scenario(scenario_file("goal37_coordinated_route_choice.yaml"));
    first_scenario.surface_operations->aircraft_routing_policy = airside::agents::AutonomyMode::RollingHorizonCoordinated;
    second_scenario.surface_operations->aircraft_routing_policy = airside::agents::AutonomyMode::RollingHorizonCoordinated;
    const auto first = airside::Simulation{std::move(first_scenario), 42}.run();
    const auto second = airside::Simulation{std::move(second_scenario), 42}.run();
    EXPECT_EQ(first.agent_decisions, second.agent_decisions);
    EXPECT_EQ(first.events, second.events);
    EXPECT_EQ(first.metrics.agent_objectives_failed, 0U);
    EXPECT_EQ(first.metrics.surface_safe_failures, 0U);
    EXPECT_EQ(first.metrics.surface_aircraft_aircraft_collisions, 0U);
    EXPECT_EQ(first.metrics.surface_aircraft_ground_collisions, 0U);
    ASSERT_FALSE(first.agent_decisions.empty());
    EXPECT_TRUE(std::ranges::all_of(first.agent_decisions, [](const auto& event) {
        return event.policy_id == "rolling-horizon-coordinated-v1" &&
            event.planning_horizon == airside::SimTime{600} && !event.coordination_rule.empty() &&
            !event.safety_constraints.empty() && !event.replan_trigger.empty();
    }));
    EXPECT_TRUE(std::ranges::any_of(first.agent_decisions, [](const auto& event) {
        return std::ranges::any_of(event.candidates, [](const auto& candidate) {
            return !candidate.predicted_contention.empty() && candidate.estimated_system_cost_seconds >= candidate.score_seconds;
        });
    }));
}

TEST(SurfaceTrafficTest, CounterfactualRolloutsAreIsolatedAndReplayDeterministically) {
    auto first_scenario = airside::load_scenario(scenario_file("goal37_coordinated_route_choice.yaml"));
    auto second_scenario = airside::load_scenario(scenario_file("goal37_coordinated_route_choice.yaml"));
    first_scenario.surface_operations->aircraft_routing_policy = airside::agents::AutonomyMode::Counterfactual;
    second_scenario.surface_operations->aircraft_routing_policy = airside::agents::AutonomyMode::Counterfactual;
    const auto first = airside::Simulation{std::move(first_scenario), 42}.run();
    const auto second = airside::Simulation{std::move(second_scenario), 42}.run();
    EXPECT_EQ(first.agent_decisions, second.agent_decisions);
    EXPECT_EQ(first.events, second.events);
    EXPECT_EQ(first.metrics.agent_objectives_failed, 0U);
    EXPECT_EQ(first.metrics.surface_safe_failures, 0U);
    EXPECT_EQ(first.metrics.surface_aircraft_aircraft_collisions, 0U);
    EXPECT_EQ(first.metrics.surface_aircraft_ground_collisions, 0U);
    ASSERT_FALSE(first.agent_decisions.empty());
    EXPECT_TRUE(std::ranges::all_of(first.agent_decisions, [](const auto& event) {
        return event.policy_id == "counterfactual-operational-planning-v1" &&
            event.planning_horizon == airside::SimTime{600} &&
            !event.state_identifier.empty() && !event.safety_constraints.empty();
    }));
    EXPECT_TRUE(std::ranges::any_of(first.agent_decisions, [](const auto& event) {
        return std::ranges::any_of(event.candidates, [](const auto& candidate) {
            return candidate.counterfactual_evaluated && candidate.rollout_events > 0U;
        });
    }));
    EXPECT_TRUE(std::ranges::any_of(first.agent_decisions, [](const auto& event) {
        if (!event.observation.reservation_blocked) return false;
        const auto hold = std::ranges::find(event.candidates, airside::agents::CandidateKind::Wait,
            &airside::agents::CandidateEvaluation::kind);
        const auto current = std::ranges::find(event.candidates, airside::agents::CandidateKind::ContinueCurrentRoute,
            &airside::agents::CandidateEvaluation::kind);
        return hold != event.candidates.end() && current != event.candidates.end() &&
            hold->counterfactual_evaluated && current->counterfactual_evaluated &&
            hold->estimated_system_cost_seconds > 0.0 &&
            hold->rollout_wait >= hold->expected_wait && current->rollout_wait >= hold->expected_wait;
    }));
}

TEST(SurfaceTrafficTest, SyntheticBottleneckRolloutCanTradeTaxiDistanceForLessDownstreamWait) {
    auto scenario = airside::load_scenario(scenario_file("surface_traffic.yaml"));
    scenario.surface_operations->aircraft_routing_policy = airside::agents::AutonomyMode::Counterfactual;
    const auto result = airside::Simulation{std::move(scenario), 42}.run();
    const auto decision = std::ranges::find_if(result.agent_decisions, [](const auto& event) {
        if (!event.differs_from_rolling_horizon || !event.observation.reservation_blocked ||
            !event.selected_candidate || *event.selected_candidate >= event.candidates.size()) return false;
        const auto& selected = event.candidates[*event.selected_candidate];
        return selected.kind == airside::agents::CandidateKind::AlternateRoute && selected.counterfactual_evaluated;
    });
    ASSERT_NE(decision, result.agent_decisions.end());
    const auto current = std::ranges::find(decision->candidates, airside::agents::CandidateKind::ContinueCurrentRoute,
        &airside::agents::CandidateEvaluation::kind);
    ASSERT_NE(current, decision->candidates.end());
    const auto& selected = decision->candidates[*decision->selected_candidate];
    EXPECT_LT(selected.rollout_wait, current->rollout_wait);
    EXPECT_GT(selected.rollout_taxi_time, current->rollout_taxi_time);
    EXPECT_GT(selected.rollout_distance_m, current->rollout_distance_m);
    EXPECT_EQ(selected.rollout_completed_objectives, current->rollout_completed_objectives);
}

TEST(SurfaceTrafficTest, CostAwareFixturesRejectWastefulAcceptWorthwhileAndProtectAgedWaiter) {
    using namespace airside;
    const auto run_cost_aware = [](const char* name) {
        auto scenario = load_scenario(scenario_file(name));
        scenario.surface_operations->aircraft_routing_policy = agents::AutonomyMode::CostAwareCounterfactual;
        return Simulation{std::move(scenario), 42}.run();
    };
    const auto decision_for = [](const auto& result, std::string_view aircraft, SimTime time) {
        return std::ranges::find_if(result.agent_decisions, [&](const auto& event) {
            return event.agent_id == aircraft && event.time == time && event.cost_aware_evaluated &&
                event.selected_candidate && *event.selected_candidate < event.candidates.size();
        });
    };

    // Fixture A: Goal 39 sees only the 33-second wait saving, while the route
    // adds 96 seconds of taxi and 96 metres. The actual Goal 40 rollout rejects it.
    const auto wasteful = run_cost_aware("surface_traffic_disrupted.yaml");
    const auto wasteful_decision = decision_for(wasteful, "aircraft-2", SimTime{848});
    ASSERT_NE(wasteful_decision, wasteful.agent_decisions.end());
    const auto& wasteful_selected = wasteful_decision->candidates[*wasteful_decision->selected_candidate];
    ASSERT_EQ(wasteful_selected.kind, agents::CandidateKind::ContinueCurrentRoute);
    const auto wasteful_alt = std::ranges::find_if(wasteful_decision->candidates, [](const auto& candidate) {
        return candidate.kind == agents::CandidateKind::AlternateRoute && candidate.counterfactual_evaluated &&
            candidate.rollout_wait == SimTime::zero() && candidate.rollout_taxi_time == SimTime{256};
    });
    ASSERT_NE(wasteful_alt, wasteful_decision->candidates.end());
    EXPECT_EQ(wasteful_selected.rollout_wait, SimTime{33});
    EXPECT_FALSE(wasteful_alt->material_improvement_passed);
    EXPECT_FALSE(wasteful_alt->bounded_regret_passed);

    // Fixture B: a 122-second reduction in predicted blocking pays for the
    // 80-second, 80-metre detour in a real simulator decision.
    const auto worthwhile = run_cost_aware("goal35_route_choice_demo.yaml");
    const auto worthwhile_decision = decision_for(worthwhile, "aircraft-3", SimTime{578});
    ASSERT_NE(worthwhile_decision, worthwhile.agent_decisions.end());
    const auto& worthwhile_selected = worthwhile_decision->candidates[*worthwhile_decision->selected_candidate];
    ASSERT_EQ(worthwhile_selected.kind, agents::CandidateKind::AlternateRoute);
    const auto worthwhile_base = std::ranges::find_if(worthwhile_decision->candidates, [](const auto& candidate) {
        return candidate.kind == agents::CandidateKind::ContinueCurrentRoute && candidate.counterfactual_evaluated;
    });
    ASSERT_NE(worthwhile_base, worthwhile_decision->candidates.end());
    EXPECT_EQ(worthwhile_base->rollout_wait - worthwhile_selected.rollout_wait, SimTime{122});
    EXPECT_EQ(worthwhile_selected.rollout_taxi_time - worthwhile_base->rollout_taxi_time, SimTime{80});
    EXPECT_EQ(worthwhile_selected.rollout_distance_m - worthwhile_base->rollout_distance_m, 80.0);
    EXPECT_TRUE(worthwhile_selected.material_improvement_passed);
    EXPECT_TRUE(worthwhile_selected.bounded_regret_passed);

    // Fixture C reuses a genuine long-corridor rollout. The short-term model
    // retains the route; age and priority make the same safe alternative
    // admissible, exercising the policy's starvation protection.
    const auto fairness = run_cost_aware("goal37_long_shared_corridor.yaml");
    const auto fairness_decision = decision_for(fairness, "aircraft-1", SimTime{801});
    ASSERT_NE(fairness_decision, fairness.agent_decisions.end());
    agents::CostAwareCounterfactualPolicy policy;
    const auto ordinary = policy.decide(fairness_decision->observation, fairness_decision->objective, {});
    ASSERT_EQ(ordinary.action, agents::ActionType::FollowRoute);
    auto aged_observation = fairness_decision->observation;
    aged_observation.accumulated_wait_age = SimTime{120};
    auto priority_objective = fairness_decision->objective;
    priority_objective.priority = 10;
    agents::evaluate_operational_costs(aged_observation, priority_objective);
    const auto protected_decision = policy.decide(aged_observation, priority_objective, {});
    ASSERT_EQ(protected_decision.action, agents::ActionType::PlanRoute);
    EXPECT_NE(protected_decision.route, fairness_decision->observation.current_route);
    const auto protected_alt = std::ranges::find_if(aged_observation.candidates, [](const auto& candidate) {
        return candidate.kind == agents::CandidateKind::AlternateRoute && candidate.material_improvement_passed &&
            candidate.bounded_regret_passed;
    });
    ASSERT_NE(protected_alt, aged_observation.candidates.end());
    EXPECT_EQ(protected_alt->rollout_wait, SimTime::zero());
    EXPECT_EQ(fairness_decision->observation.candidates[0].rollout_wait, SimTime{57});

    for (const auto* result : {&wasteful, &worthwhile, &fairness}) {
        EXPECT_EQ(result->metrics.agent_objectives_failed, 0U);
        EXPECT_EQ(result->metrics.surface_safe_failures, 0U);
        EXPECT_EQ(result->metrics.surface_aircraft_aircraft_collisions, 0U);
        EXPECT_EQ(result->metrics.surface_aircraft_ground_collisions, 0U);
        EXPECT_TRUE(std::ranges::all_of(result->agent_decisions, [](const auto& event) {
            return !event.cost_aware_evaluated || !event.safety_constraints.empty();
        }));
    }
}

TEST(SurfaceTrafficTest, AdaptiveRouteChoiceUsesForecastCongestionAndRecordsOutcome) {
    auto adaptive_scenario = airside::load_scenario(scenario_file("goal35_route_choice_demo.yaml"));
    auto scripted_scenario = airside::load_scenario(scenario_file("goal35_route_choice_demo.yaml"));
    adaptive_scenario.surface_operations->aircraft_routing_policy = airside::agents::AutonomyMode::Adaptive;
    scripted_scenario.surface_operations->aircraft_routing_policy = airside::agents::AutonomyMode::Scripted;
    const auto adaptive = airside::Simulation{std::move(adaptive_scenario), 42}.run();
    const auto scripted = airside::Simulation{std::move(scripted_scenario), 42}.run();
    const auto choice = [](const auto& result) {
        return std::ranges::find_if(result.agent_decisions, [](const auto& event) {
            return event.agent_id == "aircraft-3" && event.time == airside::SimTime{548} &&
                    event.action == airside::agents::ActionType::PlanRoute;
        });
    };
    const auto adaptive_choice = choice(adaptive);
    const auto scripted_choice = choice(scripted);
    ASSERT_NE(adaptive_choice, adaptive.agent_decisions.end());
    ASSERT_NE(scripted_choice, scripted.agent_decisions.end());
    ASSERT_TRUE(adaptive_choice->selected_route);
    ASSERT_TRUE(scripted_choice->selected_route);
    EXPECT_NE(adaptive_choice->selected_route->nodes, scripted_choice->selected_route->nodes);
    EXPECT_GT(adaptive_choice->selected_route->distance_m, scripted_choice->selected_route->distance_m);
    const auto aircraft_metrics = [](const auto& result) {
        return std::ranges::find(result.metrics.aircraft, "AX303", &airside::AircraftMetrics::flight_number);
    };
    const auto adaptive_aircraft = aircraft_metrics(adaptive);
    const auto scripted_aircraft = aircraft_metrics(scripted);
    ASSERT_NE(adaptive_aircraft, adaptive.metrics.aircraft.end());
    ASSERT_NE(scripted_aircraft, scripted.metrics.aircraft.end());
    EXPECT_LT(adaptive_aircraft->surface_departure_time, scripted_aircraft->surface_departure_time);
    EXPECT_EQ(adaptive.metrics.surface_safe_failures, 0U);
    EXPECT_EQ(scripted.metrics.surface_safe_failures, 0U);
    EXPECT_EQ(adaptive.metrics.surface_aircraft_aircraft_collisions, 0U);
    EXPECT_EQ(scripted.metrics.surface_aircraft_aircraft_collisions, 0U);
}

TEST(SurfaceTrafficTest, CoordinatedChoiceReducesTaxiTimeOnSyntheticSharedRouteContention) {
    for (const std::uint32_t seed : {42U, 43U, 44U}) {
        auto adaptive_scenario = airside::load_scenario(scenario_file("goal37_coordinated_route_choice.yaml"));
        auto coordinated_scenario = airside::load_scenario(scenario_file("goal37_coordinated_route_choice.yaml"));
        adaptive_scenario.surface_operations->aircraft_routing_policy = airside::agents::AutonomyMode::Adaptive;
        coordinated_scenario.surface_operations->aircraft_routing_policy = airside::agents::AutonomyMode::CoordinatedAdaptive;
        const auto adaptive = airside::Simulation{std::move(adaptive_scenario), seed}.run();
        const auto coordinated = airside::Simulation{std::move(coordinated_scenario), seed}.run();

        const auto find_first_choice = [](const auto& result) {
            return std::ranges::find_if(result.agent_decisions, [](const auto& event) {
                return event.agent_id == "aircraft-3" &&
                    event.action == airside::agents::ActionType::PlanRoute;
            });
        };
        const auto adaptive_choice = find_first_choice(adaptive);
        const auto coordinated_choice = find_first_choice(coordinated);
        ASSERT_NE(adaptive_choice, adaptive.agent_decisions.end());
        ASSERT_NE(coordinated_choice, coordinated.agent_decisions.end());
        ASSERT_TRUE(adaptive_choice->selected_route);
        ASSERT_TRUE(coordinated_choice->selected_route);
        EXPECT_NE(adaptive_choice->selected_route->edges, coordinated_choice->selected_route->edges);
        EXPECT_TRUE(std::ranges::any_of(coordinated_choice->candidates, [](const auto& candidate) {
            return candidate.affected_agents > 0 && candidate.estimated_wait_imposed_seconds > 0.0;
        }));
        EXPECT_EQ(coordinated_choice->reason, airside::agents::ReasonCode::Congestion);
        EXPECT_NE(coordinated_choice->explanation.find("lower local-cost alternative"), std::string::npos);
        EXPECT_EQ(coordinated_choice->safety_constraints.empty(), false);

        EXPECT_EQ(adaptive.metrics.surface_wait_seconds, coordinated.metrics.surface_wait_seconds);
        EXPECT_EQ(adaptive.metrics.mean_departure_delay_seconds, coordinated.metrics.mean_departure_delay_seconds);
        EXPECT_EQ(adaptive.metrics.surface_taxi_seconds - coordinated.metrics.surface_taxi_seconds, 6);
        for (const auto* result : {&adaptive, &coordinated}) {
            EXPECT_EQ(result->metrics.agent_objectives_failed, 0U);
            EXPECT_EQ(result->metrics.surface_safe_failures, 0U);
            EXPECT_EQ(result->metrics.surface_aircraft_aircraft_collisions, 0U);
            EXPECT_EQ(result->metrics.surface_aircraft_ground_collisions, 0U);
        }
    }
}

TEST(SurfaceTrafficTest, OperatorClosureIsObservedAndRecordedByAffectedAgent) {
    auto scenario = airside::load_scenario(scenario_file("surface_traffic_disrupted.yaml"));
    scenario.road_events.clear();
    scenario.surface_operations->aircraft_routing_policy = airside::agents::AutonomyMode::Adaptive;
    airside::Simulation simulation{std::move(scenario), 42};
    bool intervention_scheduled = false;
    std::size_t steps = 0;
    while (!simulation.finished() && steps < 10000U) {
        if (!intervention_scheduled) {
            const auto decision = std::ranges::find_if(simulation.event_history(), [](const auto& event) {
                return event.type == airside::SimulationEventType::AgentDecision &&
                    event.decision == "PLAN_ROUTE" && event.route && !event.route->edges.empty();
            });
            if (decision != simulation.event_history().end()) {
                simulation.schedule_surface_availability(decision->route->edges.front(), false);
                intervention_scheduled = true;
            }
        }
        (void)simulation.advance();
        ++steps;
    }
    ASSERT_TRUE(intervention_scheduled);
    ASSERT_TRUE(simulation.finished());
    const auto result = simulation.result();
    EXPECT_EQ(result.metrics.surface_departed_aircraft, 3U);
    EXPECT_EQ(result.metrics.surface_safe_failures, 0U);
    EXPECT_TRUE(std::ranges::any_of(result.agent_decisions, [](const auto& event) {
        return event.operator_triggered && event.reason == airside::agents::ReasonCode::OperatorIntervention &&
            event.action == airside::agents::ActionType::PlanRoute;
    }));
    EXPECT_EQ(result.metrics.agent_operator_interventions, 1U);
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
    const auto first_arrival = std::ranges::find(scenario.aircraft, airside::AircraftId{4}, &airside::Aircraft::id);
    ASSERT_NE(first_arrival, scenario.aircraft.end());
    scenario.aircraft.emplace_back(airside::AircraftId{99}, "SECOND-ARRIVAL",
        first_arrival->scheduled_arrival() + airside::SimTime{120},
        airside::SimTime{10000}, first_arrival->gate(), first_arrival->gate_node(),
        std::vector<airside::ServiceTask>{}, "", std::nullopt,
        airside::AircraftOperationType::ArrivalOnly, exit->id);
    airside::Simulation simulation(std::move(scenario), 42);
    std::size_t steps = 0;
    constexpr std::size_t maximum_steps = 10000;
    const auto second_failure_recorded = [&] {
        return std::ranges::any_of(simulation.event_history(), [](const auto& event) {
            return event.type == airside::SimulationEventType::SurfaceSafeFailure &&
                event.aircraft == airside::AircraftId{99};
        });
    };
    while (!simulation.finished() && !second_failure_recorded() && steps < maximum_steps) {
        (void)simulation.advance();
        ++steps;
    }
    const auto& events = simulation.event_history();
    EXPECT_EQ(std::ranges::count_if(events, [](const auto& event) {
        return event.type == airside::SimulationEventType::SurfaceSafeFailure && event.aircraft &&
            (*event.aircraft == airside::AircraftId{4} || *event.aircraft == airside::AircraftId{99});
    }), 2);
    const auto failure = std::ranges::find_if(events, [](const auto& event) {
        return event.type == airside::SimulationEventType::SurfaceSafeFailure &&
            event.aircraft == airside::AircraftId{4};
    });
    ASSERT_NE(failure, events.end());
    EXPECT_EQ(failure->detail, "no available taxi route from runway exit to assigned arrival gate");
    EXPECT_NE(airside::format_event(*failure).find("no available taxi route from runway exit to assigned arrival gate"),
        std::string::npos);
    EXPECT_TRUE(second_failure_recorded())
        << "second arrival remained blocked by the failed aircraft's stale runway-exit reservation after "
        << steps << " events at simulated second " << simulation.current_time().count();
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
