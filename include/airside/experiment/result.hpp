#pragma once

#include "airside/experiment/case_generator.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace airside::experiment {

struct TaskRunMetrics {
    std::uint32_t task_id{0};
    std::string service_type;
    std::string state;
    std::int64_t requested_at_seconds{-1};
    std::int64_t started_at_seconds{-1};
    std::int64_t completed_at_seconds{-1};
    std::int64_t waiting_seconds{0};
    std::string required_resource;
    std::string assigned_resource;
    constexpr auto operator<=>(const TaskRunMetrics&) const = default;
};

struct AircraftRunMetrics {
    std::string flight_number;
    double turnaround_seconds{0.0};
    double departure_delay_seconds{0.0};
    double service_waiting_seconds{0.0};
    std::string turnaround_id;
    double estimated_ready_time_seconds{0.0};
    double actual_completion_time_seconds{0.0};
    double schedule_slack_seconds{0.0};
    std::string critical_path_task_ids;
    std::vector<TaskRunMetrics> tasks;

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
    std::size_t total_turnarounds{0};
    std::size_t completed_turnarounds{0};
    std::size_t delayed_turnarounds{0};
    std::size_t failed_or_timed_out_turnarounds{0};
    double maximum_turnaround_seconds{0.0};
    double maximum_departure_delay_seconds{0.0};
    std::size_t on_time_departures{0};
    double on_time_departure_rate{0.0};
    double total_service_task_wait_seconds{0.0};
    double maximum_service_task_wait_seconds{0.0};
    std::size_t task_reassignments{0};
    std::size_t disruption_triggered_replans{0};
    std::size_t unresolved_service_requests{0};
    std::size_t fleet_collisions{0};
    double fleet_minimum_separation_m{0.0};
    std::size_t fleet_reservation_requests{0};
    std::size_t fleet_reservation_contentions{0};
    std::size_t fleet_outstanding_reservations{0};
    std::size_t fleet_unfinished_requests{0};
    std::size_t fleet_reassignments{0};
    std::size_t fleet_requests_created{0};
    std::size_t fleet_requests_completed{0};
    std::size_t fleet_requests_failed{0};
    std::vector<std::pair<std::string, double>> resource_utilization;
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
