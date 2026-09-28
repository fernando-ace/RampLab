#pragma once
#include "airside/autonomy/simulation.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>
namespace airside::autonomy {
struct AutonomyRunRequest { AutonomyScenario scenario; std::uint64_t seed{}; std::size_t ordinal{}; };
struct AutonomyExperimentRun { std::uint64_t seed{}; std::size_t ordinal{}; double gnss_sigma_m{}; MissionMetrics metrics{}; };
struct AutonomyExperimentReport { std::vector<AutonomyExperimentRun> runs; std::size_t worker_count{}; std::chrono::duration<double> wall_time{}; };
[[nodiscard]] AutonomyExperimentReport execute_runs(std::vector<AutonomyRunRequest> requests,std::size_t workers);
}
