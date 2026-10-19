#include "airside/operations/simulation.hpp"
#include "airside/experiment/case_generator.hpp"
#include "airside/scenario/scenario_loader.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace airside::test {
namespace {

std::filesystem::path scenario_path(const char* name) {
    return std::filesystem::path{AIRSIDE_SOURCE_DIR} / "scenarios" / name;
}

TEST(TurnaroundTest, IndependentTasksOverlapAndDependenciesWaitForPrerequisites) {
    Simulation simulation{load_scenario(scenario_path("turnaround_normal.yaml")), 42};
    std::vector<SimulationEventRecord> events;
    struct Sink final : ISimulationEventSink {
        std::vector<SimulationEventRecord>& events;
        explicit Sink(std::vector<SimulationEventRecord>& value) : events(value) {}
        void on_event(const SimulationEventRecord& event) noexcept override { events.push_back(event); }
    } sink{events};
    simulation.add_event_sink(sink);
    const auto result = simulation.run();

    const auto event_for = [&](SimulationEventType type, TaskId task) -> const SimulationEventRecord& {
        const auto found = std::ranges::find_if(events, [&](const auto& event) {
            return event.type == type && event.task == task;
        });
        EXPECT_NE(found, events.end());
        return *found;
    };
    const auto& deboarding_started = event_for(SimulationEventType::TurnaroundTaskStarted, TaskId{1});
    const auto& fuel_dispatched = event_for(SimulationEventType::TurnaroundTaskDispatched, TaskId{2});
    const auto& deboarding_completed = event_for(SimulationEventType::TurnaroundTaskCompleted, TaskId{1});
    const auto& baggage_started = event_for(SimulationEventType::TurnaroundTaskStarted, TaskId{4});
    EXPECT_LT(deboarding_started.timestamp, fuel_dispatched.timestamp);
    EXPECT_LT(fuel_dispatched.timestamp, deboarding_completed.timestamp);
    EXPECT_LE(deboarding_completed.timestamp, baggage_started.timestamp);
    EXPECT_EQ(result.metrics.total_turnarounds, 1U);
    EXPECT_EQ(result.metrics.completed_turnarounds, 1U);
    EXPECT_EQ(result.metrics.aircraft.front().departure_delay, std::chrono::seconds{0});
    EXPECT_EQ(result.metrics.disruption_triggered_replans, 0U);
    EXPECT_EQ(result.metrics.unresolved_service_requests, 0U);
    EXPECT_GT(result.metrics.fuel_utilization, 0.0);
    EXPECT_GT(result.metrics.baggage_utilization, 0.0);
    EXPECT_FALSE(result.metrics.aircraft.front().critical_path_tasks.empty());
    const auto& timings = result.metrics.aircraft.front().task_timings;
    const auto baggage_load = std::ranges::find(timings, TaskId{6}, &AircraftMetrics::TaskTiming::task);
    ASSERT_NE(baggage_load, timings.end());
    EXPECT_EQ(baggage_load->requested_at, baggage_load->started_at);
    EXPECT_GE(result.metrics.aircraft.front().critical_path_tasks.size(), 2U);
    EXPECT_TRUE(std::ranges::is_sorted(events, {}, &SimulationEventRecord::sequence));
}

TEST(TurnaroundTest, ResourceContentionCompletesAllAircraftDeterministically) {
    const auto scenario = load_scenario(scenario_path("turnaround_contention.yaml"));
    Simulation first{scenario, 42};
    Simulation second{scenario, 42};
    const auto first_result = first.run();
    const auto second_result = second.run();
    EXPECT_EQ(first_result.events, second_result.events);
    EXPECT_EQ(first_result.metrics.aircraft, second_result.metrics.aircraft);
    EXPECT_EQ(first_result.metrics.total_turnarounds, 3U);
    EXPECT_EQ(first_result.metrics.completed_turnarounds, 3U);
    EXPECT_GT(first_result.metrics.total_service_task_wait_seconds, 0);
    EXPECT_EQ(first_result.metrics.task_reassignments, 0U);
    EXPECT_EQ(first_result.metrics.unresolved_service_requests, 0U);
    EXPECT_EQ(first_result.metrics.fleet_collisions, 0U);
    EXPECT_EQ(first_result.metrics.fleet_requests_created, 9U);
    EXPECT_EQ(first_result.metrics.fleet_requests_completed, 9U);
    EXPECT_EQ(first_result.metrics.fleet_requests_failed, 0U);
    EXPECT_EQ(first_result.metrics.aircraft.size(), 3U);
    EXPECT_TRUE(std::ranges::all_of(first_result.metrics.aircraft, [](const auto& aircraft) {
        return aircraft.actual_completion_time.has_value() && aircraft.departure_delay == SimTime::zero() &&
            std::ranges::all_of(aircraft.task_timings, [](const auto& task) {
                return task.state == TaskStatus::Completed && task.completed_at.has_value();
            });
    }));
    const auto first_snapshot = Simulation{scenario, 42}.snapshot();
    ASSERT_EQ(first_snapshot.gates.size(), 3U);
    EXPECT_TRUE(std::ranges::all_of(first_snapshot.gates, [](const auto& gate) {
        return !gate.occupying_aircraft.has_value();
    }));
}

TEST(TurnaroundTest, StaggeredFlightBankCompletesThreeIndependentAircraftSafely) {
    const auto scenario = load_scenario(scenario_path("turnaround_flight_bank.yaml"));
    Simulation live{scenario, 42};
    while (live.next_event_time() && *live.next_event_time() <= SimTime{1300}) {
        ASSERT_TRUE(live.advance());
    }
    const auto occupied_snapshot = live.snapshot();
    ASSERT_EQ(occupied_snapshot.gates.size(), 3U);
    std::vector<AircraftId> gate_occupants;
    for (const auto& gate : occupied_snapshot.gates) {
        ASSERT_TRUE(gate.occupying_aircraft.has_value());
        gate_occupants.push_back(*gate.occupying_aircraft);
    }
    std::ranges::sort(gate_occupants);
    EXPECT_EQ(std::ranges::unique(gate_occupants).begin(), gate_occupants.end());

    Simulation first{scenario, 42};
    Simulation second{scenario, 42};
    const auto first_result = first.run();
    const auto second_result = second.run();
    EXPECT_EQ(first_result.events, second_result.events);
    EXPECT_EQ(first_result.metrics.aircraft, second_result.metrics.aircraft);
    EXPECT_EQ(first_result.metrics.total_turnarounds, 3U);
    EXPECT_EQ(first_result.metrics.completed_turnarounds, 3U);
    EXPECT_EQ(first_result.metrics.fleet_collisions, 0U);
    EXPECT_EQ(first_result.metrics.fleet_requests_created, 9U);
    EXPECT_EQ(first_result.metrics.fleet_requests_completed, 9U);
    EXPECT_EQ(first_result.metrics.fleet_requests_failed, 0U);
    EXPECT_EQ(first_result.metrics.unresolved_service_requests, 0U);
    EXPECT_TRUE(std::ranges::all_of(first_result.metrics.aircraft, [](const auto& aircraft) {
        return aircraft.actual_completion_time.has_value() && aircraft.departure_delay == SimTime::zero() &&
            std::ranges::all_of(aircraft.task_timings, [](const auto& task) {
                return task.state == TaskStatus::Completed && task.completed_at.has_value();
            });
    }));
    std::vector<TaskId> completed_tasks;
    for (const auto& event : first_result.events) {
        if (event.type == SimulationEventType::TurnaroundTaskCompleted) completed_tasks.push_back(*event.task);
    }
    EXPECT_EQ(completed_tasks.size(), 18U);
    std::ranges::sort(completed_tasks);
    EXPECT_EQ(std::ranges::unique(completed_tasks).begin(), completed_tasks.end());
    EXPECT_TRUE(std::ranges::all_of(first.snapshot().turnarounds, [&](const auto& turnaround) {
        return std::ranges::all_of(turnaround.tasks, [&turnaround](const auto& task) {
            return task.status == TaskStatus::Completed && task.completed_at.has_value() &&
                std::ranges::all_of(task.prerequisites, [&](TaskId prerequisite) {
                    return std::ranges::any_of(turnaround.tasks, [&](const auto& prior) {
                        return prior.id == prerequisite && prior.status == TaskStatus::Completed &&
                            prior.completed_at <= task.started_at;
                    });
                });
        });
    }));
}

TEST(TurnaroundTest, MultiAircraftTaskDurationDisruptionDelaysDeterministicallyWithoutCollisions) {
    const auto scenario = load_scenario(scenario_path("turnaround_flight_bank_disrupted.yaml"));
    Simulation first{scenario, 42};
    Simulation second{scenario, 42};
    const auto first_result = first.run();
    const auto second_result = second.run();
    EXPECT_EQ(first_result.events, second_result.events);
    EXPECT_EQ(first_result.metrics.aircraft, second_result.metrics.aircraft);
    EXPECT_EQ(first_result.metrics.total_turnarounds, 3U);
    EXPECT_EQ(first_result.metrics.completed_turnarounds, 3U);
    EXPECT_EQ(first_result.metrics.delayed_turnarounds, 2U);
    EXPECT_GT(first_result.metrics.aircraft[0].departure_delay, SimTime::zero());
    EXPECT_GT(first_result.metrics.aircraft[1].departure_delay, SimTime::zero());
    EXPECT_EQ(first_result.metrics.aircraft[2].departure_delay, SimTime::zero());
    EXPECT_EQ(first_result.metrics.disruption_triggered_replans, 1U);
    EXPECT_EQ(first_result.metrics.fleet_collisions, 0U);
    EXPECT_EQ(first_result.metrics.fleet_requests_completed, 9U);
    EXPECT_EQ(first_result.metrics.fleet_requests_failed, 0U);
    EXPECT_EQ(first_result.metrics.unresolved_service_requests, 0U);
}

TEST(TurnaroundTest, FlightBankReassignsOutagedVehicleSafelyAndDeterministically) {
    auto scenario = load_scenario(scenario_path("turnaround_flight_bank_outage.yaml"));
    auto control_scenario = scenario;
    control_scenario.vehicle_outages.clear();
    std::erase_if(control_scenario.vehicles, [](const auto& vehicle) {
        return vehicle.name() == "BaggageCart-Backup";
    });
    const auto control = Simulation{control_scenario, 42}.run();

    Simulation first{scenario, 42};
    Simulation second{scenario, 42};
    Simulation discarded_history{scenario, 42, SimulationHistoryPolicy::Discard};
    experiment::ScenarioOverrides overrides;
    overrides.fuel_truck_count = 1;
    Simulation experiment_override{experiment::apply_overrides(scenario, overrides), 42,
        SimulationHistoryPolicy::Discard};
    const auto first_result = first.run();
    const auto second_result = second.run();
    const auto discarded_history_result = discarded_history.run();
    const auto experiment_override_result = experiment_override.run();
    EXPECT_EQ(first_result.events, second_result.events);
    EXPECT_EQ(first_result.metrics.aircraft, second_result.metrics.aircraft);
    EXPECT_EQ(first_result.metrics.aircraft, discarded_history_result.metrics.aircraft);
    EXPECT_EQ(first_result.metrics.fleet_requests_completed,
        discarded_history_result.metrics.fleet_requests_completed);
    EXPECT_EQ(first_result.metrics.fleet_requests_failed,
        discarded_history_result.metrics.fleet_requests_failed);
    EXPECT_EQ(first_result.metrics.aircraft, experiment_override_result.metrics.aircraft);
    EXPECT_EQ(first_result.metrics.fleet_requests_failed,
        experiment_override_result.metrics.fleet_requests_failed);
    ASSERT_EQ(first_result.metrics.aircraft.size(), 3U);
    EXPECT_EQ(first_result.metrics.total_turnarounds, 3U);
    EXPECT_EQ(first_result.metrics.completed_turnarounds, 3U);
    EXPECT_EQ(first_result.metrics.failed_or_timed_out_turnarounds, 0U);
    EXPECT_EQ(first_result.metrics.fleet_collisions, 0U);
    EXPECT_EQ(first_result.metrics.fleet_outstanding_reservations, 0U);
    EXPECT_EQ(first_result.metrics.fleet_unfinished_requests, 0U);
    EXPECT_EQ(first_result.metrics.fleet_requests_created, 9U);
    EXPECT_EQ(first_result.metrics.fleet_requests_completed, 9U);
    EXPECT_EQ(first_result.metrics.fleet_requests_failed, 0U);
    EXPECT_EQ(first_result.metrics.fleet_reassignments, 1U);
    EXPECT_EQ(first_result.metrics.task_reassignments, 1U);
    EXPECT_EQ(first_result.metrics.disruption_triggered_replans, 1U);
    EXPECT_EQ(first_result.metrics.unresolved_service_requests, 0U);
    EXPECT_TRUE(std::ranges::all_of(first_result.metrics.aircraft, [](const auto& aircraft) {
        return aircraft.actual_completion_time.has_value() && aircraft.departure_delay > SimTime::zero() &&
            std::ranges::all_of(aircraft.task_timings, [](const auto& task) {
                return task.state == TaskStatus::Completed && task.completed_at.has_value();
            });
    }));

    const auto outage = std::ranges::find_if(first_result.events, [](const auto& event) {
        return event.type == SimulationEventType::TurnaroundVehicleUnavailable;
    });
    const auto reassignment = std::ranges::find_if(first_result.events, [](const auto& event) {
        return event.type == SimulationEventType::TurnaroundTaskReassigned;
    });
    ASSERT_NE(outage, first_result.events.end());
    ASSERT_NE(reassignment, first_result.events.end());
    EXPECT_EQ(outage->timestamp, SimTime{130});
    EXPECT_EQ(outage->vehicle, VehicleId{2});
    EXPECT_EQ(outage->task, TaskId{3});
    EXPECT_EQ(reassignment->task, outage->task);
    EXPECT_EQ(reassignment->vehicle, VehicleId{3});
    EXPECT_LE(outage->sequence, reassignment->sequence);
    const auto replacement_task = std::ranges::find(first_result.metrics.aircraft.front().task_timings,
        TaskId{3}, &AircraftMetrics::TaskTiming::task);
    ASSERT_NE(replacement_task, first_result.metrics.aircraft.front().task_timings.end());
    EXPECT_EQ(replacement_task->assigned_resource, "BaggageCart-Backup");
    EXPECT_EQ(replacement_task->started_at, SimTime{168});
    EXPECT_EQ(replacement_task->completed_at, SimTime{408});

    std::vector<TaskId> completed_tasks;
    std::size_t departures{};
    for (const auto& event : first_result.events) {
        if (event.type == SimulationEventType::TurnaroundTaskCompleted) completed_tasks.push_back(*event.task);
        if (event.type == SimulationEventType::AircraftDeparted) ++departures;
    }
    EXPECT_EQ(completed_tasks.size(), 18U);
    std::ranges::sort(completed_tasks);
    EXPECT_EQ(std::ranges::unique(completed_tasks).begin(), completed_tasks.end());
    EXPECT_EQ(departures, 3U);

    const auto outage_snapshot = first.snapshot();
    const auto disabled_vehicle = std::ranges::find(outage_snapshot.vehicles, VehicleId{2},
        &ServiceVehicleSnapshot::id);
    ASSERT_NE(disabled_vehicle, outage_snapshot.vehicles.end());
    EXPECT_EQ(disabled_vehicle->fleet_status, "Unavailable");
    const auto safe_bay = std::ranges::find(outage_snapshot.road_nodes, "Baggage Cart Safe Bay",
        &RoadNodeSnapshot::name);
    ASSERT_NE(safe_bay, outage_snapshot.road_nodes.end());
    EXPECT_EQ(disabled_vehicle->current_node, safe_bay->id);

    std::int64_t outage_delay_delta{};
    for (std::size_t i = 0; i < first_result.metrics.aircraft.size(); ++i) {
        EXPECT_EQ(first_result.metrics.aircraft[i].departure_delay,
            std::max(SimTime::zero(), *first_result.metrics.aircraft[i].actual_completion_time -
                scenario.aircraft[i].scheduled_departure()));
        outage_delay_delta += first_result.metrics.aircraft[i].departure_delay.count() -
            control.metrics.aircraft[i].departure_delay.count();
    }
    EXPECT_EQ(outage_delay_delta, 73);
}

TEST(TurnaroundTest, RecoveryActionAdapterReassignsOutagedTurnaroundTaskThroughFleet) {
    auto scenario = load_scenario(scenario_path("turnaround_flight_bank_outage.yaml"));
    scenario.recovery_mode = RecoveryMode::Integrated;
    Simulation simulation{scenario, 42};
    std::size_t steps = 0;
    while (simulation.recovery_epochs().empty() && simulation.advance() && ++steps < 1000) {}
    ASSERT_FALSE(simulation.recovery_epochs().empty());
    const auto action = RecoveryAction{RecoveryActionKind::ReassignVehicle, "3", "3", "test adapter"};
    const auto validation = simulation.validate_recovery_action(action);
    ASSERT_TRUE(validation.accepted) << validation.reason;
    const auto applied = simulation.apply_recovery_action(action);
    ASSERT_TRUE(applied.accepted) << applied.reason;
    const auto accepted_action_replay = simulation.replay_to_recovery_boundary();
    ASSERT_TRUE(accepted_action_replay.equivalent) << accepted_action_replay.reason;
    const auto result = simulation.run();
    const auto& metrics = result.metrics;
    EXPECT_EQ(metrics.fleet_requests_completed, 9U);
    EXPECT_EQ(metrics.fleet_requests_failed, 0U);
    EXPECT_EQ(metrics.fleet_collisions, 0U);
    const auto task = std::ranges::find(metrics.aircraft.front().task_timings,
        TaskId{3}, &AircraftMetrics::TaskTiming::task);
    ASSERT_NE(task, metrics.aircraft.front().task_timings.end());
    EXPECT_EQ(task->assigned_resource, "BaggageCart-Backup");
    EXPECT_EQ(task->state, TaskStatus::Completed);
}

TEST(TurnaroundTest, StandRecoveryAdapterReassignsScheduledAircraftWithoutTeleporting) {
    Scenario scenario;
    scenario.name = "synthetic_stand_reassignment";
    scenario.turnaround_orchestration = true;
    const NodeId stand_a_node{1}, stand_b_node{2};
    scenario.graph.add_node({stand_a_node, "stand-a", {0.0, 0.0}});
    scenario.graph.add_node({stand_b_node, "stand-b", {10.0, 0.0}});
    scenario.gates.emplace_back(GateId{1}, "A", stand_a_node);
    scenario.gates.emplace_back(GateId{2}, "B", stand_b_node);
    ServiceTask service{TaskId{1}, ServiceType::Deboarding};
    service.duration = SimTime{1};
    scenario.abstract_resource_capacity.emplace(ServiceType::Deboarding, 1U);
    scenario.aircraft.emplace_back(AircraftId{1}, "SYN001", SimTime{0}, SimTime{20}, GateId{1},
        stand_a_node, std::vector<ServiceTask>{service});
    scenario.recovery_mode = RecoveryMode::Integrated;
    Simulation simulation{scenario, 42};
    const RecoveryAction action{RecoveryActionKind::ReassignStand, "1", "2", "synthetic test"};
    EXPECT_TRUE(simulation.validate_recovery_action(action).accepted);
    const auto applied = simulation.apply_recovery_action(action);
    ASSERT_TRUE(applied.accepted) << applied.reason;
    const auto replay = simulation.replay_to_recovery_boundary();
    ASSERT_TRUE(replay.equivalent) << replay.reason;
    EXPECT_EQ(simulation.snapshot().aircraft.front().assigned_gate, GateId{2});
    EXPECT_EQ(simulation.snapshot().aircraft.front().state, AircraftState::Scheduled);
    const auto result = simulation.run();
    EXPECT_EQ(result.aircraft.front().gate(), GateId{2});
    EXPECT_EQ(result.aircraft.front().state(), AircraftState::Departed);
    EXPECT_EQ(simulation.snapshot().gates[1].occupying_aircraft, std::nullopt);
}

TEST(TurnaroundTest, ResequenceRecoveryChangesNormalSchedulerOrderAndReplays) {
    Scenario scenario;
    scenario.name = "synthetic_task_resequence";
    scenario.turnaround_orchestration = true;
    const NodeId stand{1};
    scenario.graph.add_node({stand, "stand", {0.0, 0.0}});
    scenario.gates.emplace_back(GateId{1}, "A", stand);
    ServiceTask task_a{TaskId{1}, ServiceType::Deboarding}; task_a.duration = SimTime{2};
    ServiceTask task_b{TaskId{2}, ServiceType::Deboarding}; task_b.duration = SimTime{2};
    scenario.abstract_resource_capacity.emplace(ServiceType::Deboarding, 1U);
    scenario.aircraft.emplace_back(AircraftId{1}, "SYN001", SimTime{0}, SimTime{20}, GateId{1}, stand,
        std::vector<ServiceTask>{task_a, task_b});
    scenario.recovery_mode = RecoveryMode::Integrated;
    Simulation ordinary{scenario, 42};
    const auto ordinary_result = ordinary.run();
    std::vector<TaskId> ordinary_started;
    for (const auto& event : ordinary_result.events)
        if (event.type == SimulationEventType::TurnaroundTaskStarted) ordinary_started.push_back(*event.task);
    EXPECT_EQ(ordinary_started, (std::vector<TaskId>{TaskId{1}, TaskId{2}}));
    Simulation simulation{scenario, 42};
    const RecoveryAction action{RecoveryActionKind::ResequenceTasks, "2", "1", "promote task 2"};
    ASSERT_TRUE(simulation.validate_recovery_action(action).accepted);
    ASSERT_TRUE(simulation.apply_recovery_action(action).accepted);
    const auto replay = simulation.replay_to_recovery_boundary();
    ASSERT_TRUE(replay.equivalent) << replay.reason;
    const auto result = simulation.run();
    std::vector<TaskId> started;
    for (const auto& event : result.events)
        if (event.type == SimulationEventType::TurnaroundTaskStarted) started.push_back(*event.task);
    ASSERT_EQ(started.size(), 2U);
    EXPECT_EQ(started, (std::vector<TaskId>{TaskId{2}, TaskId{1}}));
    EXPECT_TRUE(std::ranges::all_of(result.aircraft.front().tasks(), [](const auto& task) {
        return task.status == TaskStatus::Completed && task.completed_at.has_value();
    }));
    EXPECT_LE(result.aircraft.front().task(TaskId{2}).started_at.value(),
        result.aircraft.front().task(TaskId{2}).completed_at.value());
}

TEST(TurnaroundTest, ResequenceRecoveryRejectsMissingSameAndDependencyInvalidTasks) {
    Scenario scenario;
    scenario.turnaround_orchestration = true;
    const NodeId stand{1}; scenario.graph.add_node({stand, "stand", {0.0, 0.0}});
    scenario.gates.emplace_back(GateId{1}, "A", stand);
    ServiceTask task_a{TaskId{1}, ServiceType::Deboarding};
    ServiceTask task_b{TaskId{2}, ServiceType::Fueling}; task_b.prerequisites = {TaskId{1}};
    ServiceTask task_c{TaskId{3}, ServiceType::Catering}; task_c.prerequisites = {TaskId{2}};
    scenario.aircraft.emplace_back(AircraftId{1}, "SYN001", SimTime{10}, SimTime{30}, GateId{1}, stand,
        std::vector<ServiceTask>{task_a, task_b, task_c});
    Simulation simulation{scenario, 42};
    EXPECT_EQ(simulation.validate_recovery_action({RecoveryActionKind::ResequenceTasks, "99", "1", {}}).reason,
        "promoted_task_not_found");
    EXPECT_EQ(simulation.validate_recovery_action({RecoveryActionKind::ResequenceTasks, "1", "99", {}}).reason,
        "target_task_not_found");
    EXPECT_EQ(simulation.validate_recovery_action({RecoveryActionKind::ResequenceTasks, "1", "1", {}}).reason,
        "same_task_cannot_be_resequenced");
    EXPECT_EQ(simulation.validate_recovery_action({RecoveryActionKind::ResequenceTasks, "2", "1", {}}).reason,
        "resequence_violates_prerequisite");
    EXPECT_EQ(simulation.validate_recovery_action({RecoveryActionKind::ResequenceTasks, "3", "1", {}}).reason,
        "resequence_violates_prerequisite");
}

TEST(TurnaroundTest, ResequenceRecoveryRejectsCompletedCommittedAndCrossTurnaroundTasks) {
    Scenario scenario; scenario.turnaround_orchestration = true;
    const NodeId stand{1}; scenario.graph.add_node({stand, "stand", {0.0, 0.0}});
    scenario.gates.emplace_back(GateId{1}, "A", stand);
    ServiceTask a{TaskId{1}, ServiceType::Deboarding}; a.status = TaskStatus::Completed;
    ServiceTask b{TaskId{2}, ServiceType::Fueling};
    ServiceTask c{TaskId{3}, ServiceType::Catering}; c.status = TaskStatus::InProgress;
    ServiceTask d{TaskId{4}, ServiceType::LavatoryService};
    scenario.aircraft.emplace_back(AircraftId{1}, "SYN001", SimTime{10}, SimTime{30}, GateId{1}, stand,
        std::vector<ServiceTask>{a, b, c});
    scenario.aircraft.emplace_back(AircraftId{2}, "SYN002", SimTime{20}, SimTime{40}, GateId{1}, stand,
        std::vector<ServiceTask>{d});
    Simulation simulation{scenario, 42};
    EXPECT_EQ(simulation.validate_recovery_action({RecoveryActionKind::ResequenceTasks, "1", "2", {}}).reason,
        "promoted_task_completed");
    EXPECT_EQ(simulation.validate_recovery_action({RecoveryActionKind::ResequenceTasks, "2", "1", {}}).reason,
        "target_task_completed");
    EXPECT_EQ(simulation.validate_recovery_action({RecoveryActionKind::ResequenceTasks, "3", "2", {}}).reason,
        "promoted_task_already_committed");
    EXPECT_EQ(simulation.validate_recovery_action({RecoveryActionKind::ResequenceTasks, "2", "3", {}}).reason,
        "target_task_order_already_committed");
    EXPECT_EQ(simulation.validate_recovery_action({RecoveryActionKind::ResequenceTasks, "4", "2", {}}).reason,
        "tasks_in_different_turnarounds");
}

TEST(TurnaroundTest, IntegratedResequenceSelectsBetterOrderAndShadowOnlyRecommends) {
    const auto make_scenario = [] {
        Scenario scenario; scenario.name = "integrated_resequence";
        scenario.turnaround_orchestration = true;
        const NodeId stand{1}; scenario.graph.add_node({stand, "stand", {0.0, 0.0}});
        scenario.gates.emplace_back(GateId{1}, "A", stand);
        ServiceTask slow{TaskId{1}, ServiceType::Deboarding}; slow.duration = SimTime{100};
        ServiceTask parallel{TaskId{2}, ServiceType::Deboarding}; parallel.duration = SimTime{1000};
        ServiceTask dependent{TaskId{3}, ServiceType::CabinCleaning}; dependent.duration = SimTime{1000};
        dependent.status = TaskStatus::Blocked;
        dependent.prerequisites = {TaskId{2}};
        scenario.abstract_resource_capacity[ServiceType::Deboarding] = 1;
        scenario.abstract_resource_capacity[ServiceType::CabinCleaning] = 1;
        scenario.aircraft.emplace_back(AircraftId{1}, "SYN001", SimTime{10}, SimTime{1500}, GateId{1}, stand,
            std::vector<ServiceTask>{slow, parallel, dependent});
        scenario.task_duration_disruptions.push_back({SimTime{0}, TaskId{1}, SimTime{1000}});
        return scenario;
    };
    const auto started = [](const SimulationResult& result) {
        std::vector<TaskId> order;
        for (const auto& event : result.events)
            if (event.type == SimulationEventType::TurnaroundTaskStarted) order.push_back(*event.task);
        return order;
    };
    auto reactive_scenario = make_scenario(); reactive_scenario.recovery_mode = RecoveryMode::Reactive;
    auto shadow_scenario = make_scenario(); shadow_scenario.recovery_mode = RecoveryMode::Shadow;
    auto integrated_scenario = make_scenario(); integrated_scenario.recovery_mode = RecoveryMode::Integrated;
    auto repeated_integrated_scenario = make_scenario(); repeated_integrated_scenario.recovery_mode = RecoveryMode::Integrated;
    Simulation reactive{reactive_scenario, 42};
    Simulation shadow{shadow_scenario, 42};
    Simulation integrated{integrated_scenario, 42};
    Simulation repeated_integrated{repeated_integrated_scenario, 42};
    const auto reactive_result = reactive.run();
    const auto shadow_result = shadow.run();
    const auto integrated_result = integrated.run();
    const auto repeated_result = repeated_integrated.run();
    EXPECT_EQ(reactive_result.events, shadow_result.events);
    EXPECT_EQ(reactive.snapshot(), shadow.snapshot());
    EXPECT_EQ(reactive_result.metrics, shadow_result.metrics);
    EXPECT_EQ(started(reactive_result), (std::vector<TaskId>{TaskId{1}, TaskId{2}, TaskId{3}}));
    ASSERT_FALSE(shadow.recovery_epochs().empty());
    const auto& shadow_epoch = shadow.recovery_epochs().front();
    EXPECT_EQ(shadow_epoch.trigger, "task_duration_slowdown");
    EXPECT_EQ(shadow_epoch.selected_candidate_id, std::optional<std::string>{"resequence:2:1"});
    EXPECT_TRUE(std::ranges::all_of(shadow.recovery_epochs(), [](const auto& epoch) {
        return epoch.application_result != "accepted";
    }));
    EXPECT_EQ(started(shadow_result), started(reactive_result));
    ASSERT_FALSE(integrated.recovery_epochs().empty());
    const auto& epoch = integrated.recovery_epochs().front();
    EXPECT_EQ(epoch.selected_candidate_id, std::optional<std::string>{"resequence:2:1"});
    const auto evaluated = std::ranges::find_if(epoch.evaluations, [](const auto& evaluation) {
        return evaluation.candidate.id == "resequence:2:1";
    });
    ASSERT_NE(evaluated, epoch.evaluations.end());
    EXPECT_TRUE(evaluated->valid) << evaluated->reason;
    EXPECT_LT(evaluated->outcome.worst_departure_delay_seconds.value(),
        epoch.evaluations.front().outcome.worst_departure_delay_seconds.value());
    EXPECT_EQ(epoch.application_result, "accepted");
    EXPECT_EQ(started(integrated_result), (std::vector<TaskId>{TaskId{2}, TaskId{1}, TaskId{3}}));
    EXPECT_LT(integrated_result.metrics.maximum_departure_delay_seconds,
        reactive_result.metrics.maximum_departure_delay_seconds);
    EXPECT_TRUE(std::ranges::all_of(integrated_result.aircraft.front().tasks(), [](const auto& task) {
        return task.status == TaskStatus::Completed;
    }));
    const auto dependent_task = std::ranges::find(integrated_result.aircraft.front().tasks(), TaskId{3}, &ServiceTask::id);
    const auto prerequisite = std::ranges::find(integrated_result.aircraft.front().tasks(), TaskId{2}, &ServiceTask::id);
    ASSERT_NE(dependent_task, integrated_result.aircraft.front().tasks().end());
    ASSERT_NE(prerequisite, integrated_result.aircraft.front().tasks().end());
    EXPECT_GE(*dependent_task->started_at, *prerequisite->completed_at);
    EXPECT_EQ(integrated_result.events, repeated_result.events);
    ASSERT_FALSE(repeated_integrated.recovery_epochs().empty());
    EXPECT_EQ(repeated_integrated.recovery_epochs().front().candidates, epoch.candidates);
    EXPECT_EQ(repeated_integrated.recovery_epochs().front().selected_candidate_id, epoch.selected_candidate_id);
    ASSERT_EQ(repeated_integrated.recovery_epochs().front().evaluations.size(), epoch.evaluations.size());
    for (std::size_t i = 0; i < epoch.evaluations.size(); ++i) {
        EXPECT_EQ(repeated_integrated.recovery_epochs().front().evaluations[i].candidate,
            epoch.evaluations[i].candidate);
        EXPECT_EQ(repeated_integrated.recovery_epochs().front().evaluations[i].valid, epoch.evaluations[i].valid);
        EXPECT_EQ(repeated_integrated.recovery_epochs().front().evaluations[i].outcome.worst_departure_delay_seconds,
            epoch.evaluations[i].outcome.worst_departure_delay_seconds);
    }
}

TEST(TurnaroundTest, StandRecoveryAdapterRejectsInvalidAndOccupiedTargets) {
    Scenario scenario;
    scenario.name = "synthetic_stand_rejections";
    scenario.turnaround_orchestration = true;
    const NodeId a{1}, b{2}, c{3}, d{4};
    scenario.graph.add_node({a, "a", {0.0, 0.0}});
    scenario.graph.add_node({b, "b", {10.0, 0.0}});
    scenario.graph.add_node({c, "c", {20.0, 0.0}});
    scenario.graph.add_node({d, "d", {30.0, 0.0}});
    scenario.gates.emplace_back(GateId{1}, "A", a);
    scenario.gates.emplace_back(GateId{2}, "B", b, false);
    scenario.gates.emplace_back(GateId{3}, "C", c);
    scenario.gates.emplace_back(GateId{4}, "D", d);
    ServiceTask task_a{TaskId{1}, ServiceType::Deboarding};
    task_a.duration = SimTime{1};
    ServiceTask task_b{TaskId{2}, ServiceType::Deboarding};
    task_b.duration = SimTime{1};
    scenario.abstract_resource_capacity.emplace(ServiceType::Deboarding, 2U);
    scenario.aircraft.emplace_back(AircraftId{1}, "SYN001", SimTime{0}, SimTime{20}, GateId{1}, a,
        std::vector<ServiceTask>{task_a});
    scenario.aircraft.emplace_back(AircraftId{2}, "SYN002", SimTime{0}, SimTime{20}, GateId{3}, c,
        std::vector<ServiceTask>{task_b});
    Simulation simulation{scenario, 42};
    EXPECT_EQ(simulation.validate_recovery_action({RecoveryActionKind::ReassignStand, "99", "2", {}}).reason,
        "aircraft_not_found");
    EXPECT_EQ(simulation.validate_recovery_action({RecoveryActionKind::ReassignStand, "1", "99", {}}).reason,
        "gate_not_found");
    EXPECT_EQ(simulation.validate_recovery_action({RecoveryActionKind::ReassignStand, "1", "1", {}}).reason,
        "target_is_current_gate");
    EXPECT_EQ(simulation.validate_recovery_action({RecoveryActionKind::ReassignStand, "1", "2", {}}).reason,
        "gate_unavailable");
    ASSERT_TRUE(simulation.advance());
    ASSERT_TRUE(simulation.advance());
    EXPECT_EQ(simulation.validate_recovery_action({RecoveryActionKind::ReassignStand, "1", "3", {}}).reason,
        "gate_occupied");
    EXPECT_EQ(simulation.validate_recovery_action({RecoveryActionKind::ReassignStand, "1", "4", {}}).reason,
        "stand_reassignment_requires_unparked_aircraft");
}

TEST(TurnaroundTest, StandRecoveryAdapterRejectsUnreachableSurfaceTarget) {
    Scenario scenario;
    scenario.name = "synthetic_stand_route_rejection";
    scenario.turnaround_orchestration = true;
    const NodeId runway{1}, exit{2}, a{3}, b{4}, handoff{5}, depot{6};
    scenario.graph.add_node({runway, "runway", {0.0, 0.0}});
    scenario.graph.add_node({exit, "exit", {10.0, 0.0}});
    scenario.graph.add_node({a, "a", {20.0, 0.0}});
    scenario.graph.add_node({b, "b", {20.0, 10.0}});
    scenario.graph.add_node({handoff, "handoff", {30.0, 0.0}});
    scenario.graph.add_node({depot, "depot", {100.0, 100.0}});
    scenario.graph.add_edge({EdgeId{1}, exit, a, 10.0, SimTime{1}, true, false});
    scenario.gates.emplace_back(GateId{1}, "A", a);
    scenario.gates.emplace_back(GateId{2}, "B", b);
    ServiceTask task{TaskId{1}, ServiceType::Deboarding};
    task.duration = SimTime{1};
    scenario.aircraft.emplace_back(AircraftId{1}, "SYN001", SimTime{0}, SimTime{20}, GateId{1}, a,
        std::vector<ServiceTask>{task}, "SYN001", std::nullopt,
        AircraftOperationType::ArrivalTurnaround, exit);
    scenario.abstract_resource_capacity.emplace(ServiceType::Deboarding, 1U);
    scenario.surface_operations = SurfaceOperationsConfig{handoff, runway, exit};
    Simulation simulation{scenario, 42};
    EXPECT_EQ(simulation.validate_recovery_action({RecoveryActionKind::ReassignStand, "1", "2", {}}).reason,
        "stand_route_unreachable");
}

// Same outage diagnostic, with two idle recovery choices. The ordinary dispatcher
// minimizes route distance; the nearer cart encounters a timed access closure
// while the existing backup has an unaffected route. All modes share the facts.
Scenario material_gse_scenario() {
    auto scenario = load_scenario(scenario_path("turnaround_flight_bank_outage.yaml"));
    scenario.aircraft.erase(scenario.aircraft.begin() + 1, scenario.aircraft.end());
    scenario.task_duration_disruptions.clear();
    const NodeId near_depot{12};
    scenario.graph.add_node({near_depot, "Near cart depot", {100.0, 180.0}});
    scenario.graph.add_edge({EdgeId{18}, near_depot, NodeId{2}, 100.0, SimTime{200}, true, false});
    scenario.vehicles.emplace_back(VehicleId{4}, "Near baggage cart", ServiceType::Baggage,
        near_depot, 8.0);
    scenario.road_events.push_back({SimTime{131}, EdgeId{18}, false});
    scenario.road_events.push_back({SimTime{800}, EdgeId{18}, true});
    return scenario;
}

void print_gse_evaluation(const RecoveryEvaluation& evaluation) {
    const auto& outcome = evaluation.outcome;
    std::cout << "GSE private " << evaluation.candidate.id
        << " valid=" << evaluation.valid << " completed=" << outcome.completed.value_or(false)
        << " objectives=" << outcome.completed_objectives << " failures=" << outcome.objective_failures
        << " timeouts=" << outcome.timeouts << " turnaround_failures=" << outcome.turnaround_failures
        << " worst_delay=" << outcome.worst_departure_delay_seconds.value_or(-1)
        << " mean_delay=" << outcome.departure_delay_seconds.value_or(-1)
        << " service_wait=" << outcome.service_wait_seconds.value_or(-1)
        << " gse_travel_mapped=" << outcome.gse_travel_seconds.has_value()
        << " events=" << evaluation.private_events << " horizon=" << evaluation.simulated_horizon.count()
        << " termination=" << evaluation.termination_reason << "\n";
}

void check_gse_safety(const SimulationResult& result) {
    EXPECT_EQ(result.metrics.fleet_collisions, 0U);
    EXPECT_EQ(result.metrics.fleet_deadlock_count, 0U);
    EXPECT_FALSE(result.metrics.fleet_deadlock_unresolved);
    EXPECT_EQ(result.metrics.fleet_outstanding_reservations, 0U);
    EXPECT_EQ(result.metrics.fleet_unfinished_requests, 0U);
    EXPECT_EQ(result.metrics.fleet_requests_failed, 0U);
    EXPECT_EQ(result.metrics.unresolved_service_requests, 0U);
    EXPECT_EQ(result.metrics.completed_turnarounds, 1U);
    EXPECT_EQ(result.metrics.failed_or_timed_out_turnarounds, 0U);
    EXPECT_EQ(result.metrics.surface_aircraft_aircraft_collisions, 0U);
    EXPECT_EQ(result.metrics.surface_aircraft_ground_collisions, 0U);
}

void print_gse_causality(const char* label, const SimulationResult& result) {
    std::cout << "GSE " << label << " worst_delay=" << result.metrics.maximum_departure_delay_seconds
        << " aggregate_delay=" << result.metrics.aircraft.front().departure_delay.count()
        << " service_wait=" << result.metrics.total_service_task_wait_seconds << "\n";
    for (const auto& event : result.events) {
        if (event.type == SimulationEventType::TurnaroundVehicleUnavailable ||
            event.type == SimulationEventType::TurnaroundTaskReassigned ||
            event.type == SimulationEventType::TurnaroundTaskDispatched ||
            event.type == SimulationEventType::TurnaroundTaskStarted ||
            event.type == SimulationEventType::TurnaroundTaskCompleted ||
            event.type == SimulationEventType::TurnaroundTaskReady ||
            event.type == SimulationEventType::TurnaroundReadyForDeparture ||
            event.type == SimulationEventType::AircraftDeparted ||
            event.type == SimulationEventType::RoadClosed || event.type == SimulationEventType::RoadOpened)
            std::cout << label << " t=" << event.timestamp.count() << " seq=" << event.sequence << " " << format_event(event) << "\n";
    }
}

TEST(TurnaroundTest, AutomaticIntegratedGseRecoveryDiagnostic) {
    auto scenario = material_gse_scenario();
    scenario.recovery_mode = RecoveryMode::Reactive;
    Simulation simulation{scenario, 42};
    while (simulation.recovery_epochs().empty() && simulation.advance()) {}
    ASSERT_FALSE(simulation.recovery_epochs().empty());
    const auto before = simulation.snapshot();
    const auto history = simulation.event_history();
    const auto next_event = simulation.next_event_time();
    EXPECT_EQ(simulation.current_time(), SimTime{130});
    ASSERT_EQ(before.turnarounds.size(), 1U);
    const auto critical = std::ranges::find(before.turnarounds.front().tasks, TaskId{3}, &ServiceTaskSnapshot::id);
    ASSERT_NE(critical, before.turnarounds.front().tasks.end());
    EXPECT_EQ(critical->status, TaskStatus::Assigned);
    EXPECT_EQ(critical->assigned_vehicle, VehicleId{2});
    for (const auto id : {VehicleId{3}, VehicleId{4}}) {
        const auto backup = std::ranges::find(before.vehicles, id, &ServiceVehicleSnapshot::id);
        ASSERT_NE(backup, before.vehicles.end());
        EXPECT_EQ(backup->type, ServiceType::Baggage);
        EXPECT_EQ(backup->fleet_status, "Idle");
        EXPECT_FALSE(backup->assigned_aircraft);
    }
    const RecoverySearchConfig config{};
    const auto baseline = simulation.evaluate_recovery_candidate({"current_plan", 0, {}}, config);
    const RecoveryCandidate candidate{"vehicle:3:3", 1,
        {{RecoveryActionKind::ReassignVehicle, "3", "3", "replace vehicle stranded by revealed outage"}}};
    ASSERT_TRUE(simulation.validate_recovery_action(candidate.actions.front()).accepted);
    const auto replacement = simulation.evaluate_recovery_candidate(candidate, config);
    ASSERT_TRUE(baseline.valid) << baseline.reason;
    ASSERT_TRUE(replacement.valid) << replacement.reason;
    print_gse_evaluation(baseline);
    print_gse_evaluation(replacement);
    EXPECT_EQ(config.minimum_delay_improvement_seconds, 300.0);
    EXPECT_EQ(baseline.outcome.completed, true);
    EXPECT_EQ(replacement.outcome.completed, true);
    EXPECT_EQ(baseline.outcome.worst_departure_delay_seconds, 532.0);
    EXPECT_EQ(replacement.outcome.worst_departure_delay_seconds, 0.0);
    EXPECT_TRUE(recovery_outcome_materially_improves(replacement.outcome, baseline.outcome, config));
    EXPECT_EQ(simulation.snapshot(), before);
    EXPECT_EQ(simulation.event_history(), history);
    EXPECT_EQ(simulation.next_event_time(), next_event);
    const RecoveryState state{before, {{SimTime{130}, "vehicle_outage", "2", "BaggageCart-1"}}, {}};
    auto capped = config; capped.max_candidates = 2;
    const auto candidates = generate_recovery_candidates(state, capped);
    ASSERT_EQ(candidates.size(), 2U);
    EXPECT_EQ(candidates[0].id, "current_plan"); EXPECT_EQ(candidates[1], candidate);
    EXPECT_EQ(candidates, generate_recovery_candidates(state, capped));
    const auto replay = simulation.replay_to_recovery_boundary();
    EXPECT_TRUE(replay.equivalent) << replay.reason;
    // Continue against an untouched control to detect hidden parent mutations in
    // fleet reservations, queue contents, counters, or task authority.
    Simulation control{scenario, 42};
    const auto control_result = control.run();
    const auto parent_result = simulation.run();
    EXPECT_EQ(parent_result.events, control_result.events);
    EXPECT_EQ(parent_result.metrics, control_result.metrics);
    EXPECT_EQ(simulation.snapshot(), control.snapshot());
}

TEST(TurnaroundTest, IntegratedGseRecoveryMateriallyRescuesStrandedCriticalTask) {
    auto facts = material_gse_scenario();
    auto shadow_facts = facts; shadow_facts.recovery_mode = RecoveryMode::Shadow;
    auto integrated_facts = facts; integrated_facts.recovery_mode = RecoveryMode::Integrated;
    Simulation reactive{facts, 42}, shadow{shadow_facts, 42}, shadow_repeat{shadow_facts, 42};
    Simulation integrated{integrated_facts, 42}, repeat{integrated_facts, 42};
    // Compare snapshots and history at every event boundary, including movement.
    // Final metrics also compare reservation counters and safety. Recovery epochs
    // are the only separate diagnostic stream.
    std::unique_ptr<Simulation> prediction;
    while (!reactive.finished()) {
        ASSERT_EQ(reactive.next_event_time(), shadow.next_event_time());
        ASSERT_EQ(reactive.next_event_time(), shadow_repeat.next_event_time());
        ASSERT_TRUE(reactive.advance()); ASSERT_TRUE(shadow.advance()); ASSERT_TRUE(shadow_repeat.advance());
        EXPECT_EQ(reactive.snapshot(), shadow.snapshot());
        EXPECT_EQ(reactive.snapshot(), shadow_repeat.snapshot());
        EXPECT_EQ(reactive.event_history(), shadow.event_history());
        EXPECT_EQ(reactive.event_history(), shadow_repeat.event_history());
        if (!prediction && !shadow.recovery_epochs().empty()) {
            auto replay = shadow.replay_to_recovery_boundary();
            ASSERT_TRUE(replay.equivalent) << replay.reason;
            prediction = std::move(replay.simulation);
            const auto& epoch = shadow.recovery_epochs().front();
            ASSERT_EQ(epoch.selected_candidate_id, std::optional<std::string>{"vehicle:3:3"});
            const auto selected = std::ranges::find(epoch.candidates, "vehicle:3:3", &RecoveryCandidate::id);
            ASSERT_NE(selected, epoch.candidates.end());
            ASSERT_TRUE(prediction->apply_recovery_action(selected->actions.front()).accepted);
        }
    }
    ASSERT_TRUE(shadow.finished()); ASSERT_TRUE(shadow_repeat.finished()); ASSERT_TRUE(prediction);
    const auto predicted = prediction->run();
    std::optional<SimTime> travel_start, first_movement, source_at_refuge;
    std::optional<Vec2> first_backup_position;
    bool moved = false;
    while (!integrated.finished()) {
        ASSERT_EQ(integrated.next_event_time(), repeat.next_event_time());
        ASSERT_TRUE(integrated.advance()); ASSERT_TRUE(repeat.advance());
        const auto snapshot = integrated.snapshot();
        EXPECT_EQ(snapshot, repeat.snapshot());
        EXPECT_EQ(integrated.event_history(), repeat.event_history());
        const auto backup = std::ranges::find(snapshot.vehicles, VehicleId{3}, &ServiceVehicleSnapshot::id);
        ASSERT_NE(backup, snapshot.vehicles.end());
        if (!first_backup_position) first_backup_position = backup->observed_position_m;
        if (backup->fleet_status == "TravelingToAircraft" && !travel_start)
            travel_start = snapshot.simulation_time;
        if (backup->fleet_status == "TravelingToAircraft" && backup->observed_position_m != first_backup_position) {
            moved = true;
            if (!first_movement) first_movement = snapshot.simulation_time;
        }
        std::vector<VehicleId> active_owners;
        for (const auto& turnaround : snapshot.turnarounds) {
            for (const auto& task : turnaround.tasks) {
                if ((task.status != TaskStatus::Assigned && task.status != TaskStatus::InProgress) ||
                    !task.assigned_vehicle) continue;
                EXPECT_EQ(std::ranges::find(active_owners, *task.assigned_vehicle), active_owners.end());
                active_owners.push_back(*task.assigned_vehicle);
            }
        }
        if (snapshot.simulation_time >= SimTime{130}) {
            const auto source = std::ranges::find(snapshot.vehicles, VehicleId{2}, &ServiceVehicleSnapshot::id);
            ASSERT_NE(source, snapshot.vehicles.end());
            // Existing outage semantics evacuate to refuge before the public
            // snapshot reports Unavailable. The dispatcher already excludes it.
            EXPECT_TRUE(source->fleet_status == "Idle" || source->fleet_status == "Unavailable");
            EXPECT_FALSE(source->assigned_aircraft);
            if (source->fleet_status == "Unavailable" && !source_at_refuge)
                source_at_refuge = snapshot.simulation_time;
            const auto unused = std::ranges::find(snapshot.vehicles, VehicleId{4}, &ServiceVehicleSnapshot::id);
            ASSERT_NE(unused, snapshot.vehicles.end());
            EXPECT_EQ(unused->fleet_status, "Idle");
            EXPECT_FALSE(unused->assigned_aircraft);
        }
    }
    ASSERT_TRUE(repeat.finished()); ASSERT_TRUE(travel_start); EXPECT_TRUE(moved);
    ASSERT_TRUE(first_movement); ASSERT_TRUE(source_at_refuge);
    std::cout << "GSE integrated backup travel_start=" << travel_start->count()
        << " first_observed_movement=" << first_movement->count()
        << " source_unavailable_at_refuge=" << source_at_refuge->count() << "\n";
    const auto baseline = reactive.result();
    const auto live = integrated.result();
    EXPECT_EQ(baseline.metrics, shadow.result().metrics);
    EXPECT_EQ(baseline.metrics, shadow_repeat.result().metrics);
    EXPECT_EQ(live.metrics, repeat.result().metrics);
    EXPECT_EQ(live.events, predicted.events);
    EXPECT_EQ(live.metrics, predicted.metrics);
    EXPECT_EQ(integrated.snapshot(), prediction->snapshot());
    EXPECT_EQ(live.metrics.maximum_departure_delay_seconds, 0);
    EXPECT_EQ(baseline.metrics.maximum_departure_delay_seconds, 532);
    EXPECT_GE(baseline.metrics.maximum_departure_delay_seconds - live.metrics.maximum_departure_delay_seconds, 300);
    for (const auto* result : {&baseline, &live, &predicted}) check_gse_safety(*result);
    print_gse_causality("reactive", baseline);
    print_gse_causality("private", predicted);
    print_gse_causality("integrated", live);
    const auto compare_epochs = [](const Simulation& first, const Simulation& second) {
        ASSERT_EQ(first.recovery_epochs().size(), second.recovery_epochs().size());
        for (std::size_t i = 0; i < first.recovery_epochs().size(); ++i) {
            const auto& a = first.recovery_epochs()[i]; const auto& b = second.recovery_epochs()[i];
            EXPECT_EQ(a.time, b.time); EXPECT_EQ(a.trigger, b.trigger); EXPECT_EQ(a.mode, b.mode);
            EXPECT_EQ(a.fingerprint, b.fingerprint); EXPECT_EQ(a.triage, b.triage);
            EXPECT_EQ(a.candidates, b.candidates); EXPECT_EQ(a.evaluations, b.evaluations);
            EXPECT_EQ(a.selected_candidate_id, b.selected_candidate_id);
            EXPECT_EQ(a.application_result, b.application_result);
            EXPECT_EQ(a.planned, b.planned); EXPECT_EQ(a.suppressed, b.suppressed);
            EXPECT_EQ(a.skip_reason, b.skip_reason); EXPECT_EQ(a.candidate_count, b.candidate_count);
        }
    };
    compare_epochs(integrated, repeat); compare_epochs(shadow, shadow_repeat);
    ASSERT_FALSE(integrated.recovery_epochs().empty());
    const auto& epoch = integrated.recovery_epochs().front();
    const auto& shadow_epoch = shadow.recovery_epochs().front();
    EXPECT_TRUE(epoch.planned); EXPECT_EQ(epoch.trigger, "vehicle_outage"); EXPECT_EQ(epoch.time, SimTime{130});
    EXPECT_EQ(epoch.selected_candidate_id, std::optional<std::string>{"vehicle:3:3"});
    EXPECT_EQ(epoch.application_result, "accepted");
    EXPECT_EQ(shadow_epoch.candidates, epoch.candidates);
    EXPECT_EQ(shadow_epoch.evaluations, epoch.evaluations);
    EXPECT_EQ(shadow_epoch.selected_candidate_id, epoch.selected_candidate_id);
    EXPECT_EQ(shadow_epoch.fingerprint, epoch.fingerprint);
    EXPECT_TRUE(std::ranges::all_of(shadow.recovery_epochs(), [](const auto& value) {
        return value.application_result.empty();
    }));
    ASSERT_GE(epoch.evaluations.size(), 3U);
    EXPECT_EQ(epoch.candidates[0].id, "current_plan");
    EXPECT_EQ(epoch.candidates[1].id, "vehicle:3:3");
    EXPECT_EQ(epoch.candidates[2].id, "vehicle:3:4");
    ASSERT_EQ(epoch.candidates[1].actions.size(), 1U);
    EXPECT_EQ(epoch.candidates[1].actions[0].kind, RecoveryActionKind::ReassignVehicle);
    EXPECT_EQ(epoch.candidates[1].actions[0].task_id, "3");
    EXPECT_EQ(epoch.candidates[1].actions[0].target_id, "3");
    EXPECT_TRUE(epoch.evaluations[1].valid);
    EXPECT_TRUE(recovery_outcome_materially_improves(epoch.evaluations[1].outcome,
        epoch.evaluations[0].outcome, RecoverySearchConfig{}));
    EXPECT_EQ(epoch.evaluations[1].outcome.completed_objectives, live.metrics.completed_turnarounds);
    EXPECT_EQ(epoch.evaluations[1].outcome.objective_failures, live.metrics.failed_or_timed_out_turnarounds);
    EXPECT_EQ(epoch.evaluations[1].outcome.worst_departure_delay_seconds, live.metrics.maximum_departure_delay_seconds);
    EXPECT_EQ(epoch.evaluations[1].outcome.service_wait_seconds, live.metrics.total_service_task_wait_seconds);
    std::cout << "GSE epoch=" << epoch.time.count() << " fingerprint=" << epoch.fingerprint
        << " selected=" << *epoch.selected_candidate_id << " candidates=";
    for (const auto& value : epoch.candidates) std::cout << " " << value.id;
    std::cout << "\n";
    for (const auto& value : epoch.evaluations) print_gse_evaluation(value);
    // Each prerequisite must complete before its dependent starts. Each task
    // completes once; only the stranded task transfers ownership.
    for (const auto& task : live.aircraft.front().tasks()) {
        EXPECT_EQ(task.status, TaskStatus::Completed);
        EXPECT_EQ(std::ranges::count_if(live.events, [&](const auto& event) {
            return event.type == SimulationEventType::TurnaroundTaskCompleted && event.task == task.id;
        }), 1);
        for (const auto prerequisite : task.prerequisites)
            EXPECT_LE(live.aircraft.front().task(prerequisite).completed_at, task.started_at);
    }
    EXPECT_EQ(std::ranges::count_if(live.events, [](const auto& event) {
        return event.type == SimulationEventType::TurnaroundTaskReassigned;
    }), 1);
    const auto transfer = std::ranges::find_if(live.events, [](const auto& event) {
        return event.type == SimulationEventType::TurnaroundTaskReassigned;
    });
    ASSERT_NE(transfer, live.events.end());
    EXPECT_EQ(transfer->task, TaskId{3}); EXPECT_EQ(transfer->vehicle, VehicleId{3});
    EXPECT_EQ(transfer->timestamp, epoch.time);
    EXPECT_EQ(live.aircraft.front().task(TaskId{3}).assigned_vehicle, VehicleId{3});
    EXPECT_EQ(baseline.aircraft.front().task(TaskId{3}).assigned_vehicle, VehicleId{4});
    EXPECT_EQ(integrated.snapshot().vehicles[1].current_node, NodeId{10});
    EXPECT_EQ(integrated.snapshot().vehicles[1].fleet_status, "Unavailable");
    EXPECT_FALSE(std::ranges::any_of(live.events, [](const auto& event) {
        return event.timestamp >= SimTime{130} && event.vehicle == VehicleId{2} &&
            (event.type == SimulationEventType::TurnaroundTaskDispatched ||
             event.type == SimulationEventType::TurnaroundTaskStarted ||
             event.type == SimulationEventType::TurnaroundTaskCompleted);
    }));
    EXPECT_EQ(live.aircraft.front().task(TaskId{2}).assigned_vehicle, VehicleId{1});
    EXPECT_EQ(baseline.aircraft.front().task(TaskId{2}).assigned_vehicle, VehicleId{1});
    const auto replay = integrated.replay_to_recovery_boundary();
    EXPECT_TRUE(replay.equivalent) << replay.reason;
}

TEST(TurnaroundTest, LaterRecoveryEpochReplaysEarlierAcceptedAction) {
    auto scenario = load_scenario(scenario_path("turnaround_flight_bank_outage.yaml"));
    scenario.recovery_mode = RecoveryMode::Integrated;
    Simulation simulation{scenario, 42};
    while (simulation.recovery_epochs().empty() && simulation.advance()) {}
    ASSERT_FALSE(simulation.recovery_epochs().empty());
    const RecoveryAction first_action{RecoveryActionKind::ReassignVehicle, "3", "3",
        "synthetic first-epoch replay fixture"};
    const auto applied = simulation.apply_recovery_action(first_action);
    ASSERT_TRUE(applied.accepted) << applied.reason;

    while (simulation.recovery_epochs().size() < 2 && simulation.advance()) {}
    ASSERT_GE(simulation.recovery_epochs().size(), 2U);
    const auto replay = simulation.replay_to_recovery_boundary();
    ASSERT_TRUE(replay.equivalent) << replay.reason;
    const auto& later_epoch = simulation.recovery_epochs()[1];
    EXPECT_EQ(later_epoch.trigger, "task_duration_slowdown");
    ASSERT_FALSE(later_epoch.evaluations.empty());
    EXPECT_TRUE(later_epoch.evaluations.front().valid) << later_epoch.evaluations.front().reason;
}

TEST(TurnaroundTest, RecoveryShadowPlansWithoutChangingReactiveOperations) {
    auto reactive_scenario = load_scenario(scenario_path("turnaround_flight_bank_outage.yaml"));
    reactive_scenario.recovery_mode = RecoveryMode::Reactive;
    auto shadow_scenario = reactive_scenario;
    shadow_scenario.recovery_mode = RecoveryMode::Shadow;
    Simulation reactive{reactive_scenario, 42};
    Simulation shadow{shadow_scenario, 42};
    const auto reactive_result = reactive.run();
    const auto shadow_result = shadow.run();
    EXPECT_EQ(reactive_result.metrics, shadow_result.metrics);
    EXPECT_EQ(reactive.event_history(), shadow.event_history());
    EXPECT_EQ(reactive.snapshot(), shadow.snapshot());
    ASSERT_FALSE(shadow.recovery_epochs().empty());
    EXPECT_TRUE(std::ranges::all_of(shadow.recovery_epochs(), [](const auto& epoch) {
        return epoch.planned && !epoch.evaluations.empty() && !epoch.selected_candidate_id;
    }));
}

TEST(TurnaroundTest, RuntimeVehicleOutageEntersDeterministicEventStream) {
    auto scenario = load_scenario(scenario_path("turnaround_flight_bank_outage.yaml"));
    scenario.vehicle_outages.clear();
    const auto vehicle = scenario.vehicles.front().id();
    Simulation first{scenario, 42};
    Simulation second{scenario, 42};
    first.schedule_vehicle_outage(vehicle);
    second.schedule_vehicle_outage(vehicle);
    const auto a = first.run();
    const auto b = second.run();
    EXPECT_EQ(a.events, b.events);
    EXPECT_EQ(a.metrics, b.metrics);
    const auto action = std::ranges::find_if(a.events, [](const auto& event) {
        return event.type == SimulationEventType::OperatorIntervention;
    });
    ASSERT_NE(action, a.events.end());
    EXPECT_EQ(action->timestamp, SimTime::zero());
    EXPECT_EQ(action->vehicle, vehicle);
    EXPECT_NE(action->detail.find("equipment_outage"), std::string::npos);
}

TEST(TurnaroundTest, RuntimeVehicleOutageReplaysAtItsRecordedEventBoundary) {
    auto scenario = load_scenario(scenario_path("turnaround_flight_bank_outage.yaml"));
    scenario.vehicle_outages.clear();
    scenario.recovery_mode = RecoveryMode::Reactive;
    Simulation simulation{scenario, 42};
    simulation.schedule_vehicle_outage(scenario.vehicles.front().id());
    while (!std::ranges::any_of(simulation.event_history(), [](const auto& event) {
        return event.type == SimulationEventType::OperatorIntervention;
    }) && simulation.advance()) {}
    const auto parent_snapshot = simulation.snapshot();
    const auto parent_events = simulation.event_history();
    const auto first = simulation.replay_to_recovery_boundary();
    const auto second = simulation.replay_to_recovery_boundary();
    ASSERT_TRUE(first.equivalent) << first.reason;
    ASSERT_TRUE(second.equivalent) << second.reason;
    EXPECT_EQ(first.matched_events, parent_events.size());
    EXPECT_EQ(second.matched_events, first.matched_events);
    EXPECT_EQ(first.simulation->snapshot(), second.simulation->snapshot());
    EXPECT_EQ(first.simulation->event_history(), second.simulation->event_history());
    EXPECT_EQ(simulation.snapshot(), parent_snapshot);
    EXPECT_EQ(simulation.event_history(), parent_events);
}

TEST(TurnaroundTest, RuntimeSurfaceClosureReplaysAtItsRecordedEventBoundary) {
    auto scenario = load_scenario(scenario_path("turnaround_flight_bank_outage.yaml"));
    scenario.vehicle_outages.clear();
    scenario.recovery_mode = RecoveryMode::Reactive;
    const auto edge = scenario.graph.edges().front().id;
    Simulation simulation{scenario, 42};
    simulation.schedule_surface_availability(edge, false);
    while (!std::ranges::any_of(simulation.event_history(), [](const auto& event) {
        return event.type == SimulationEventType::OperatorIntervention;
    }) && simulation.advance()) {}
    const auto parent_snapshot = simulation.snapshot();
    const auto parent_events = simulation.event_history();
    const auto replay = simulation.replay_to_recovery_boundary();
    ASSERT_TRUE(replay.equivalent) << replay.reason;
    EXPECT_EQ(replay.simulation->snapshot(), parent_snapshot);
    EXPECT_EQ(replay.simulation->event_history(), parent_events);
    EXPECT_EQ(simulation.snapshot(), parent_snapshot);
    EXPECT_EQ(simulation.event_history(), parent_events);
}

TEST(TurnaroundTest, RuntimeVehicleOutageRejectsUnknownVehicle) {
    Simulation simulation{load_scenario(scenario_path("turnaround_flight_bank_outage.yaml")), 42};
    EXPECT_THROW(simulation.schedule_vehicle_outage(VehicleId{999999}), std::invalid_argument);
}

TEST(TurnaroundTest, DisruptionReplansAndChangesEstimatedReadyTimeWithoutReset) {
    Simulation simulation{load_scenario(scenario_path("turnaround_disrupted.yaml")), 42};
    SimTime before_disruption{};
    while (simulation.next_event_time() && *simulation.next_event_time() < SimTime{200}) {
        ASSERT_TRUE(simulation.advance());
    }
    before_disruption = simulation.snapshot().turnarounds.front().estimated_ready_time;
    while (simulation.next_event_time() && *simulation.next_event_time() == SimTime{200}) {
        ASSERT_TRUE(simulation.advance());
    }
    const auto after_disruption = simulation.snapshot().turnarounds.front().estimated_ready_time;
    EXPECT_GT(after_disruption, before_disruption);
    const auto result = simulation.run();
    EXPECT_EQ(result.metrics.disruption_triggered_replans, 1U);
    EXPECT_EQ(result.metrics.completed_turnarounds, 1U);
    EXPECT_TRUE(std::ranges::any_of(result.events, [](const auto& event) {
        return event.type == SimulationEventType::TurnaroundDisruptionDetected && event.timestamp == SimTime{200};
    }));
    EXPECT_EQ(result.metrics.fleet_reassignments, 1U);
    EXPECT_EQ(result.metrics.fleet_collisions, 0U);
    EXPECT_EQ(result.metrics.fleet_outstanding_reservations, 0U);
    EXPECT_TRUE(std::ranges::any_of(result.events, [](const auto& event) {
        return event.type == SimulationEventType::TurnaroundVehicleUnavailable;
    }));
    EXPECT_TRUE(std::ranges::any_of(result.events, [](const auto& event) {
        return event.type == SimulationEventType::TurnaroundTaskReassigned;
    }));
}

TEST(TurnaroundTest, UnrecoverableFleetOutageFailsSafelyWithoutDeparting) {
    std::ifstream input{scenario_path("turnaround_disrupted.yaml")};
    ASSERT_TRUE(input);
    std::string yaml{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    const auto outage = yaml.find("vehicle_outages:\n  - { time_seconds: 130, vehicle: baggage_1 }");
    ASSERT_NE(outage, std::string::npos);
    yaml.insert(outage + std::string{"vehicle_outages:\n  - { time_seconds: 130, vehicle: baggage_1 }"}.size(),
        "\n  - { time_seconds: 130, vehicle: baggage_backup }");
    const auto path = std::filesystem::temp_directory_path() / "ramplab_turnaround_no_backup_test.yaml";
    {
        std::ofstream output{path};
        ASSERT_TRUE(output);
        output << yaml;
    }
    const auto scenario = load_scenario(path);
    std::filesystem::remove(path);

    Simulation simulation{scenario, 42};
    const auto result = simulation.run();
    EXPECT_EQ(result.metrics.total_turnarounds, 1U);
    EXPECT_EQ(result.metrics.completed_turnarounds, 0U);
    EXPECT_EQ(result.metrics.failed_or_timed_out_turnarounds, 1U);
    EXPECT_EQ(result.metrics.fleet_requests_failed, 1U);
    EXPECT_EQ(result.metrics.fleet_unfinished_requests, 0U);
    EXPECT_EQ(result.metrics.unresolved_service_requests, 0U);
    EXPECT_EQ(result.metrics.fleet_collisions, 0U);
    EXPECT_EQ(simulation.snapshot().turnarounds.front().state, TurnaroundState::Failed);
    EXPECT_FALSE(std::ranges::any_of(result.events, [](const auto& event) {
        return event.type == SimulationEventType::AircraftDeparted;
    }));
}

TEST(TurnaroundTest, ScenarioLoaderRejectsCyclicTaskDependencies) {
    const auto original_path = scenario_path("turnaround_normal.yaml");
    std::ifstream input{original_path};
    ASSERT_TRUE(input);
    std::string yaml{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    const auto clean = yaml.find("id: clean");
    ASSERT_NE(clean, std::string::npos);
    const auto dependency = yaml.find("prerequisites: [deboard]", clean);
    ASSERT_NE(dependency, std::string::npos);
    yaml.replace(dependency, std::string{"prerequisites: [deboard]"}.size(), "prerequisites: [baggage_load]");
    const auto temporary = std::filesystem::temp_directory_path() / "ramplab_turnaround_cycle_test.yaml";
    {
        std::ofstream output{temporary};
        ASSERT_TRUE(output);
        output << yaml;
    }
    EXPECT_THROW(static_cast<void>(load_scenario(temporary)), ScenarioLoadError);
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
}

}  // namespace
}  // namespace airside::test
