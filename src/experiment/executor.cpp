#include "airside/experiment/executor.hpp"

#include "airside/operations/simulation.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace airside::experiment {
namespace {

RunResult execute_one(const Scenario& base, const RunRequest& request) {
    auto scenario = apply_overrides(base, request.experiment_case.overrides);
    const auto scenario_name = scenario.name;
    Simulation simulation{std::move(scenario), request.seed, SimulationHistoryPolicy::Discard};
    const auto started = std::chrono::steady_clock::now();
    const auto simulation_result = simulation.run();
    const auto elapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();

    RunResult result;
    result.ordinal = request.ordinal;
    result.case_id = request.experiment_case.id;
    result.seed = request.seed;
    result.replication = request.replication;
    result.scenario_name = scenario_name;
    result.overrides = request.experiment_case.overrides;
    result.parameters = request.experiment_case.parameters;
    result.simulated_duration_seconds = static_cast<double>(simulation_result.simulated_duration.count());
    result.execution_ms = elapsed;
    result.average_turnaround_seconds = simulation_result.metrics.average_turnaround_seconds;
    result.delayed_aircraft = simulation_result.metrics.delayed_aircraft;
    result.aircraft_count = simulation_result.metrics.aircraft.size();
    result.fuel_utilization = simulation_result.metrics.fuel_utilization;
    result.baggage_utilization = simulation_result.metrics.baggage_utilization;
    result.total_turnarounds = simulation_result.metrics.total_turnarounds;
    result.completed_turnarounds = simulation_result.metrics.completed_turnarounds;
    result.delayed_turnarounds = simulation_result.metrics.delayed_turnarounds;
    result.failed_or_timed_out_turnarounds = simulation_result.metrics.failed_or_timed_out_turnarounds;
    result.maximum_turnaround_seconds = static_cast<double>(simulation_result.metrics.maximum_turnaround_seconds);
    result.maximum_departure_delay_seconds = static_cast<double>(simulation_result.metrics.maximum_departure_delay_seconds);
    result.on_time_departures = simulation_result.metrics.on_time_departures;
    result.on_time_departure_rate = simulation_result.metrics.on_time_departure_rate;
    result.total_service_task_wait_seconds = static_cast<double>(simulation_result.metrics.total_service_task_wait_seconds);
    result.maximum_service_task_wait_seconds = static_cast<double>(simulation_result.metrics.maximum_service_task_wait_seconds);
    result.task_reassignments = simulation_result.metrics.task_reassignments;
    result.disruption_triggered_replans = simulation_result.metrics.disruption_triggered_replans;
    result.unresolved_service_requests = simulation_result.metrics.unresolved_service_requests;
    result.fleet_collisions = simulation_result.metrics.fleet_collisions;
    result.fleet_minimum_separation_m = simulation_result.metrics.fleet_minimum_separation_m;
    result.fleet_reservation_requests = simulation_result.metrics.fleet_reservation_requests;
    result.fleet_reservation_contentions = simulation_result.metrics.fleet_reservation_contentions;
    result.fleet_outstanding_reservations = simulation_result.metrics.fleet_outstanding_reservations;
    result.fleet_unfinished_requests = simulation_result.metrics.fleet_unfinished_requests;
    result.fleet_reassignments = simulation_result.metrics.fleet_reassignments;
    result.fleet_requests_created = simulation_result.metrics.fleet_requests_created;
    result.fleet_requests_completed = simulation_result.metrics.fleet_requests_completed;
    result.fleet_requests_failed = simulation_result.metrics.fleet_requests_failed;
    for (const auto& [type, utilization] : simulation_result.metrics.resource_utilization) {
        result.resource_utilization.emplace_back(std::string{to_string(type)}, utilization);
    }
    double delay_total = 0.0;
    double waiting_total = 0.0;
    result.aircraft.reserve(simulation_result.metrics.aircraft.size());
    for (const auto& aircraft : simulation_result.metrics.aircraft) {
        delay_total += static_cast<double>(aircraft.departure_delay.count());
        waiting_total += static_cast<double>(aircraft.service_waiting.count());
        auto& output = result.aircraft.emplace_back(AircraftRunMetrics{aircraft.flight_number,
            static_cast<double>(aircraft.turnaround.count()),
            static_cast<double>(aircraft.departure_delay.count()),
            static_cast<double>(aircraft.service_waiting.count())});
        output.turnaround_id = aircraft.turnaround_id;
        output.estimated_ready_time_seconds = aircraft.estimated_ready_time
            ? static_cast<double>(aircraft.estimated_ready_time->count()) : 0.0;
        output.actual_completion_time_seconds = aircraft.actual_completion_time
            ? static_cast<double>(aircraft.actual_completion_time->count()) : 0.0;
        output.schedule_slack_seconds = aircraft.schedule_slack
            ? static_cast<double>(aircraft.schedule_slack->count()) : 0.0;
        for (std::size_t index = 0; index < aircraft.critical_path_tasks.size(); ++index) {
            if (index != 0) output.critical_path_task_ids += ";";
            output.critical_path_task_ids += std::to_string(aircraft.critical_path_tasks[index].value());
        }
        for (const auto& task : aircraft.task_timings) {
            output.tasks.push_back({task.task.value(), std::string{to_string(task.service)},
                std::string{to_string(task.state)}, task.requested_at ? task.requested_at->count() : -1,
                task.started_at ? task.started_at->count() : -1, task.completed_at ? task.completed_at->count() : -1,
                task.waiting.count(), task.required_resource, task.assigned_resource});
        }
    }
    if (!result.aircraft.empty()) {
        result.average_departure_delay_seconds = simulation_result.metrics.total_turnarounds != 0
            ? simulation_result.metrics.mean_departure_delay_seconds
            : delay_total / static_cast<double>(result.aircraft.size());
        result.average_service_waiting_seconds = waiting_total / static_cast<double>(result.aircraft.size());
    }
    return result;
}

}  // namespace

ExecutionReport execute_runs(
    const Scenario& base_scenario,
    const std::vector<RunRequest>& requests,
    std::size_t worker_count,
    ProgressCallback progress) {
    if (worker_count == 0) throw std::invalid_argument("worker count must be positive");
    if (requests.empty()) throw std::invalid_argument("execution requires at least one run");
    worker_count = std::min(worker_count, requests.size());

    std::vector<RunResult> results(requests.size());
    std::atomic_size_t next{0};
    std::atomic_size_t completed{0};
    std::atomic_bool failed{false};
    std::mutex error_mutex;
    std::mutex progress_mutex;
    std::condition_variable progress_changed;
    std::exception_ptr first_error;
    const auto started = std::chrono::steady_clock::now();

    std::vector<std::jthread> workers;
    workers.reserve(worker_count);
    for (std::size_t index = 0; index < worker_count; ++index) {
        workers.emplace_back([&] {
            while (!failed.load(std::memory_order_relaxed)) {
                const auto task = next.fetch_add(1, std::memory_order_relaxed);
                if (task >= requests.size()) break;
                try {
                    results[task] = execute_one(base_scenario, requests[task]);
                    completed.fetch_add(1, std::memory_order_release);
                    progress_changed.notify_one();
                } catch (...) {
                    failed.store(true, std::memory_order_relaxed);
                    std::lock_guard lock{error_mutex};
                    if (!first_error) first_error = std::current_exception();
                    progress_changed.notify_one();
                }
            }
        });
    }

    std::size_t last_reported = 0;
    while (completed.load(std::memory_order_acquire) < requests.size() && !failed.load()) {
        const auto current = completed.load(std::memory_order_acquire);
        if (progress && current != last_reported) {
            progress({current, requests.size(), worker_count, std::chrono::steady_clock::now() - started});
            last_reported = current;
        }
        std::unique_lock lock{progress_mutex};
        progress_changed.wait_for(lock, std::chrono::milliseconds{250}, [&] {
            return completed.load(std::memory_order_acquire) != current || failed.load();
        });
    }
    workers.clear();
    if (first_error) std::rethrow_exception(first_error);
    const auto wall_time = std::chrono::duration<double>(std::chrono::steady_clock::now() - started);
    if (progress) progress({requests.size(), requests.size(), worker_count, wall_time});
    std::ranges::sort(results, {}, &RunResult::ordinal);
    return {std::move(results), worker_count, wall_time};
}

}  // namespace airside::experiment
