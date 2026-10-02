#include "airside/operations/simulation.hpp"
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
    const auto& fuel_started = event_for(SimulationEventType::TurnaroundTaskStarted, TaskId{2});
    const auto& deboarding_completed = event_for(SimulationEventType::TurnaroundTaskCompleted, TaskId{1});
    const auto& baggage_started = event_for(SimulationEventType::TurnaroundTaskStarted, TaskId{4});
    EXPECT_LT(deboarding_started.timestamp, fuel_started.timestamp);
    EXPECT_LT(fuel_started.timestamp, deboarding_completed.timestamp);
    EXPECT_LE(deboarding_completed.timestamp, baggage_started.timestamp);
    EXPECT_EQ(result.metrics.total_turnarounds, 1U);
    EXPECT_EQ(result.metrics.completed_turnarounds, 1U);
    EXPECT_EQ(result.metrics.aircraft.front().departure_delay, std::chrono::seconds{0});
    EXPECT_EQ(result.metrics.disruption_triggered_replans, 0U);
    EXPECT_EQ(result.metrics.unresolved_service_requests, 0U);
    EXPECT_FALSE(result.metrics.aircraft.front().critical_path_tasks.empty());
    const auto& timings = result.metrics.aircraft.front().task_timings;
    const auto baggage_load = std::ranges::find(timings, TaskId{6}, &AircraftMetrics::TaskTiming::task);
    ASSERT_NE(baggage_load, timings.end());
    EXPECT_EQ(baggage_load->requested_at, baggage_load->started_at);
    EXPECT_GE(result.metrics.aircraft.front().critical_path_tasks.size(), 2U);
    EXPECT_TRUE(std::ranges::is_sorted(events, {}, &SimulationEventRecord::sequence));
}

TEST(TurnaroundTest, ResourceContentionCompletesBothAircraftDeterministically) {
    const auto scenario = load_scenario(scenario_path("turnaround_contention.yaml"));
    Simulation first{scenario, 42};
    Simulation second{scenario, 42};
    const auto first_result = first.run();
    const auto second_result = second.run();
    EXPECT_EQ(first_result.events, second_result.events);
    EXPECT_EQ(first_result.metrics.aircraft, second_result.metrics.aircraft);
    EXPECT_EQ(first_result.metrics.total_turnarounds, 2U);
    EXPECT_EQ(first_result.metrics.completed_turnarounds, 2U);
    EXPECT_GT(first_result.metrics.total_service_task_wait_seconds, 0);
    EXPECT_EQ(first_result.metrics.task_reassignments, 0U);
    EXPECT_EQ(first_result.metrics.unresolved_service_requests, 0U);
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
