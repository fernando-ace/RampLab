#pragma once

#include "airside/experiment/result.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <vector>

namespace airside::experiment {

struct ExecutionProgress {
    std::size_t completed{0};
    std::size_t total{0};
    std::size_t workers{0};
    std::chrono::duration<double> elapsed{};
};

struct ExecutionReport {
    std::vector<RunResult> runs;
    std::size_t worker_count{0};
    std::chrono::duration<double> wall_time{};
};

using ProgressCallback = std::function<void(const ExecutionProgress&)>;

[[nodiscard]] ExecutionReport execute_runs(
    const Scenario& base_scenario,
    const std::vector<RunRequest>& requests,
    std::size_t worker_count,
    ProgressCallback progress = {});

}  // namespace airside::experiment
