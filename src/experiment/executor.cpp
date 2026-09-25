#include "airside/experiment/executor.hpp"

#include "airside/operations/simulation.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
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
    double delay_total = 0.0;
    double waiting_total = 0.0;
    result.aircraft.reserve(simulation_result.metrics.aircraft.size());
    for (const auto& aircraft : simulation_result.metrics.aircraft) {
        delay_total += static_cast<double>(aircraft.departure_delay.count());
        waiting_total += static_cast<double>(aircraft.service_waiting.count());
        result.aircraft.push_back({aircraft.flight_number,
            static_cast<double>(aircraft.turnaround.count()),
            static_cast<double>(aircraft.departure_delay.count()),
            static_cast<double>(aircraft.service_waiting.count())});
    }
    if (!result.aircraft.empty()) {
        result.average_departure_delay_seconds = delay_total / static_cast<double>(result.aircraft.size());
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
                } catch (...) {
                    failed.store(true, std::memory_order_relaxed);
                    std::lock_guard lock{error_mutex};
                    if (!first_error) first_error = std::current_exception();
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
        std::this_thread::sleep_for(std::chrono::milliseconds{25});
    }
    workers.clear();
    if (first_error) std::rethrow_exception(first_error);
    const auto wall_time = std::chrono::duration<double>(std::chrono::steady_clock::now() - started);
    if (progress) progress({requests.size(), requests.size(), worker_count, wall_time});
    std::ranges::sort(results, {}, &RunResult::ordinal);
    return {std::move(results), worker_count, wall_time};
}

}  // namespace airside::experiment
