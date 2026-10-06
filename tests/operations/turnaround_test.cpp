#include "airside/operations/simulation.hpp"
#include "airside/experiment/case_generator.hpp"
#include "airside/scenario/scenario_loader.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
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
