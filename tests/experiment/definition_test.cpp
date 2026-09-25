#include "airside/experiment/case_generator.hpp"

#include <gtest/gtest.h>

#include <set>

namespace airside::experiment {
namespace {

ExperimentDefinition definition() {
    ExperimentDefinition value;
    value.name = "test";
    value.scenario_path = "scenario.yaml";
    value.seeds.values = {7, 8};
    value.parameters = {
        {ParameterKey::FuelTruckCount, {std::int64_t{1}, std::int64_t{2}}},
        {ParameterKey::RoadClosureEnabled, {true, false}},
    };
    value.workers = {false, 2};
    return value;
}

TEST(ExperimentDefinitionTest, GeneratesStableCartesianProductAndIds) {
    const auto first = generate_cases(definition());
    const auto second = generate_cases(definition());
    ASSERT_EQ(first.size(), 4U);
    ASSERT_EQ(second.size(), first.size());
    EXPECT_EQ(first.front().id, "case_0001");
    EXPECT_EQ(first.back().id, "case_0004");
    for (std::size_t index = 0; index < first.size(); ++index) {
        EXPECT_EQ(first[index].id, second[index].id);
        EXPECT_EQ(first[index].parameters, second[index].parameters);
    }
    EXPECT_EQ(first[0].overrides.fuel_truck_count, 1U);
    EXPECT_EQ(first[0].overrides.road_closure_enabled, true);
    EXPECT_EQ(first[1].overrides.road_closure_enabled, false);
    EXPECT_EQ(first[2].overrides.fuel_truck_count, 2U);
}

TEST(ExperimentDefinitionTest, ExpandsSeedsAndReplicationsDeterministically) {
    auto value = definition();
    value.seeds.replications = 2;
    const auto cases = generate_cases(value);
    const auto runs = generate_runs(value, cases);
    ASSERT_EQ(runs.size(), 16U);
    EXPECT_EQ(runs[0].seed, 7U);
    EXPECT_EQ(runs[0].replication, 1U);
    EXPECT_EQ(runs[1].replication, 2U);
    EXPECT_EQ(runs[2].seed, 8U);
    EXPECT_EQ(runs[4].experiment_case.id, "case_0002");
    for (std::size_t index = 0; index < runs.size(); ++index) EXPECT_EQ(runs[index].ordinal, index);
}

TEST(ExperimentDefinitionTest, RejectsInvalidDefinitionsAndOverrideValues) {
    auto value = definition();
    value.seeds.values.clear();
    EXPECT_THROW({ [[maybe_unused]] const auto ignored = generate_cases(value); }, std::invalid_argument);
    value = definition();
    value.workers = {false, 0};
    EXPECT_THROW({ [[maybe_unused]] const auto ignored = generate_cases(value); }, std::invalid_argument);
    value = definition();
    value.parameters[0].values = {std::int64_t{0}};
    EXPECT_THROW({ [[maybe_unused]] const auto ignored = generate_cases(value); }, std::invalid_argument);
}

TEST(ExperimentDefinitionTest, AppliesTypedOverridesWithoutMutatingBaseScenario) {
    Scenario base;
    base.name = "base";
    base.service_durations[ServiceType::Fueling] = SimTime{10};
    base.service_durations[ServiceType::Baggage] = SimTime{20};
    base.vehicles.emplace_back(VehicleId{1}, "fuel", ServiceType::Fueling, NodeId{1}, 5.0);
    base.vehicles.emplace_back(VehicleId{2}, "bag", ServiceType::Baggage, NodeId{1}, 4.0);
    base.aircraft.emplace_back(AircraftId{1}, "AX1", SimTime{100}, SimTime{200}, GateId{1}, NodeId{2},
        std::vector<ServiceTask>{{TaskId{1}, ServiceType::Fueling}, {TaskId{2}, ServiceType::Baggage}});
    base.road_events.push_back({SimTime{50}, EdgeId{1}, false});
    ScenarioOverrides overrides;
    overrides.fuel_truck_count = 3;
    overrides.fuel_service_duration = SimTime{30};
    overrides.fuel_vehicle_speed_mps = 7.0;
    overrides.aircraft_arrival_offset = SimTime{5};
    overrides.aircraft_departure_offset = SimTime{10};
    overrides.road_closure_enabled = false;
    overrides.road_closure_time = SimTime{60};
    const auto changed = apply_overrides(base, overrides);
    EXPECT_EQ(base.vehicles.size(), 2U);
    EXPECT_EQ(base.aircraft.front().scheduled_arrival(), SimTime{100});
    EXPECT_EQ(base.road_events.size(), 1U);
    EXPECT_EQ(changed.vehicles.size(), 4U);
    EXPECT_EQ(changed.vehicles.front().speed_mps(), 7.0);
    EXPECT_EQ(changed.service_durations.at(ServiceType::Fueling), SimTime{30});
    EXPECT_EQ(changed.aircraft.front().scheduled_arrival(), SimTime{105});
    EXPECT_EQ(changed.aircraft.front().scheduled_departure(), SimTime{210});
    EXPECT_TRUE(changed.road_events.empty());
}

}  // namespace
}  // namespace airside::experiment
