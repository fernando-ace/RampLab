#pragma once

#include "airside/experiment/case_generator.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace airside::experiment {

struct AircraftRunMetrics {
    std::string flight_number;
    double turnaround_seconds{0.0};
    double departure_delay_seconds{0.0};
    double service_waiting_seconds{0.0};

    constexpr auto operator<=>(const AircraftRunMetrics&) const = default;
};

struct RunResult {
    std::size_t ordinal{0};
    std::string case_id;
    std::uint64_t seed{0};
    std::size_t replication{1};
    std::string scenario_name;
    ScenarioOverrides overrides;
    std::vector<std::pair<ParameterKey, ParameterValue>> parameters;
    double simulated_duration_seconds{0.0};
    double execution_ms{0.0};
    double average_turnaround_seconds{0.0};
    double average_departure_delay_seconds{0.0};
    double average_service_waiting_seconds{0.0};
    std::size_t delayed_aircraft{0};
    std::size_t aircraft_count{0};
    double fuel_utilization{0.0};
    double baggage_utilization{0.0};
    std::vector<AircraftRunMetrics> aircraft;
};

struct DistributionStatistics {
    std::size_t count{0};
    double mean{0.0};
    double minimum{0.0};
    double maximum{0.0};
    double standard_deviation{0.0};
    double median{0.0};
    double p50{0.0};
    double p90{0.0};
    double p95{0.0};
};

struct CaseSummary {
    ExperimentCase experiment_case;
    DistributionStatistics average_turnaround_seconds;
    DistributionStatistics average_departure_delay_seconds;
    DistributionStatistics average_service_waiting_seconds;
    DistributionStatistics fuel_utilization;
    DistributionStatistics baggage_utilization;
    double mean_delayed_aircraft{0.0};
    double probability_any_delay{0.0};
};

[[nodiscard]] DistributionStatistics calculate_statistics(std::vector<double> values);
[[nodiscard]] std::vector<CaseSummary> summarize_results(
    const std::vector<ExperimentCase>& cases,
    const std::vector<RunResult>& runs);

}  // namespace airside::experiment
