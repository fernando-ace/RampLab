#include "airside/experiment/executor.hpp"
#include "airside/scenario/scenario_loader.hpp"

#include <gtest/gtest.h>

#include <set>
#include <tuple>

namespace airside::experiment {
namespace {

ExperimentDefinition small_definition() {
    ExperimentDefinition definition;
    definition.name = "parallel_test";
    definition.scenario_path = std::filesystem::path{AIRSIDE_SOURCE_DIR} / "scenarios" / "baseline.yaml";
    definition.seeds.values = {40, 41, 42};
    definition.parameters = {{ParameterKey::FuelTruckCount, {std::int64_t{1}, std::int64_t{2}}}};
    definition.workers = {false, 2};
    return definition;
}

auto deterministic_projection(const RunResult& run) {
    return std::tuple{run.ordinal, run.case_id, run.seed, run.replication, run.scenario_name,
        run.simulated_duration_seconds, run.average_turnaround_seconds,
        run.average_departure_delay_seconds, run.average_service_waiting_seconds,
        run.delayed_aircraft, run.aircraft_count, run.fuel_utilization,
        run.baggage_utilization, run.aircraft};
}

TEST(ExperimentExecutorTest, ExecutesEveryRunExactlyOnceWithDeterministicOrdering) {
    const auto definition = small_definition();
    const auto cases = generate_cases(definition);
    const auto requests = generate_runs(definition, cases);
    const auto scenario = load_scenario(definition.scenario_path);
    const auto report = execute_runs(scenario, requests, 4);
    ASSERT_EQ(report.runs.size(), 6U);
    EXPECT_EQ(report.worker_count, 4U);
    std::set<std::tuple<std::string, std::uint64_t, std::size_t>> identities;
    for (std::size_t index = 0; index < report.runs.size(); ++index) {
        EXPECT_EQ(report.runs[index].ordinal, index);
        identities.emplace(report.runs[index].case_id, report.runs[index].seed, report.runs[index].replication);
    }
    EXPECT_EQ(identities.size(), report.runs.size());
}

TEST(ExperimentExecutorTest, OneAndMultipleWorkersProduceIdenticalSimulationResultsRepeatedly) {
    const auto definition = small_definition();
    const auto cases = generate_cases(definition);
    const auto requests = generate_runs(definition, cases);
    const auto scenario = load_scenario(definition.scenario_path);
    const auto serial = execute_runs(scenario, requests, 1);
    for (int repetition = 0; repetition < 5; ++repetition) {
        const auto parallel = execute_runs(scenario, requests, 4);
        ASSERT_EQ(parallel.runs.size(), serial.runs.size());
        for (std::size_t index = 0; index < serial.runs.size(); ++index) {
            EXPECT_EQ(deterministic_projection(parallel.runs[index]), deterministic_projection(serial.runs[index]));
        }
    }
}

TEST(ExperimentExecutorTest, BaselineAndHighCapacitySeed42MatchKnownMetrics) {
    const auto run_scenario = [](std::string_view filename) {
        const auto scenario_path = std::filesystem::path{AIRSIDE_SOURCE_DIR} / "scenarios" / filename;
        const auto scenario = load_scenario(scenario_path);
        ExperimentDefinition definition;
        definition.name = "regression";
        definition.scenario_path = scenario_path;
        definition.seeds.values = {42};
        const auto cases = generate_cases(definition);
        return execute_runs(scenario, generate_runs(definition, cases), 1).runs.front();
    };
    const auto baseline = run_scenario("baseline.yaml");
    EXPECT_DOUBLE_EQ(baseline.average_turnaround_seconds, 30.0 * 60.0);
    EXPECT_EQ(baseline.delayed_aircraft, 1U);
    EXPECT_DOUBLE_EQ(baseline.fuel_utilization, 0.88);
    EXPECT_DOUBLE_EQ(baseline.baggage_utilization, 1.0);
    const auto high_capacity = run_scenario("high_capacity.yaml");
    EXPECT_DOUBLE_EQ(high_capacity.average_turnaround_seconds, 26.0 * 60.0);
    EXPECT_EQ(high_capacity.delayed_aircraft, 0U);
    EXPECT_NEAR(high_capacity.fuel_utilization, 0.628571428571, 1e-12);
    EXPECT_NEAR(high_capacity.baggage_utilization, 0.714285714286, 1e-12);
}

TEST(ExperimentExecutorTest, DiscardPolicyDoesNotRetainEventHistory) {
    auto scenario = load_scenario(std::filesystem::path{AIRSIDE_SOURCE_DIR} / "scenarios" / "baseline.yaml");
    Simulation simulation{std::move(scenario), 42, SimulationHistoryPolicy::Discard};
    const auto result = simulation.run();
    EXPECT_TRUE(result.events.empty());
    EXPECT_TRUE(result.event_log.empty());
    EXPECT_EQ(result.metrics.aircraft.size(), 3U);
}

}  // namespace
}  // namespace airside::experiment
