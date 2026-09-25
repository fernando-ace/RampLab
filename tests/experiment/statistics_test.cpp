#include "airside/experiment/result.hpp"

#include <gtest/gtest.h>

namespace airside::experiment {
namespace {

TEST(ExperimentStatisticsTest, CalculatesStablePopulationStatisticsAndNearestRankPercentiles) {
    const auto stats = calculate_statistics({1.0, 2.0, 3.0, 4.0, 100.0});
    EXPECT_EQ(stats.count, 5U);
    EXPECT_DOUBLE_EQ(stats.mean, 22.0);
    EXPECT_DOUBLE_EQ(stats.minimum, 1.0);
    EXPECT_DOUBLE_EQ(stats.maximum, 100.0);
    EXPECT_NEAR(stats.standard_deviation, 39.0128184063, 1e-9);
    EXPECT_DOUBLE_EQ(stats.median, 3.0);
    EXPECT_DOUBLE_EQ(stats.p50, 3.0);
    EXPECT_DOUBLE_EQ(stats.p90, 100.0);
    EXPECT_DOUBLE_EQ(stats.p95, 100.0);
}

TEST(ExperimentStatisticsTest, MedianAveragesTwoMiddleValues) {
    const auto stats = calculate_statistics({4.0, 1.0, 3.0, 2.0});
    EXPECT_DOUBLE_EQ(stats.median, 2.5);
    EXPECT_DOUBLE_EQ(stats.p50, 2.0);
}

TEST(ExperimentStatisticsTest, RejectsEmptyInput) {
    EXPECT_THROW({ [[maybe_unused]] const auto ignored = calculate_statistics({}); }, std::invalid_argument);
}

TEST(ExperimentStatisticsTest, AggregatesDelayProbabilityPerCase) {
    ExperimentCase experiment_case{0, "case_0001", {}, {}};
    RunResult first; first.case_id = experiment_case.id; first.average_turnaround_seconds = 10.0;
    first.average_departure_delay_seconds = 2.0; first.average_service_waiting_seconds = 3.0;
    first.delayed_aircraft = 0; first.fuel_utilization = 0.5; first.baggage_utilization = 0.6;
    auto second = first; second.average_turnaround_seconds = 20.0; second.delayed_aircraft = 2;
    const auto result = summarize_results({experiment_case}, {first, second});
    ASSERT_EQ(result.size(), 1U);
    EXPECT_DOUBLE_EQ(result.front().average_turnaround_seconds.mean, 15.0);
    EXPECT_DOUBLE_EQ(result.front().mean_delayed_aircraft, 1.0);
    EXPECT_DOUBLE_EQ(result.front().probability_any_delay, 0.5);
}

}  // namespace
}  // namespace airside::experiment
