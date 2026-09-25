#pragma once

#include "airside/experiment/executor.hpp"

#include <chrono>
#include <filesystem>
#include <string_view>

namespace airside::experiment {

void write_experiment_outputs(
    const ExperimentDefinition& definition,
    const std::vector<ExperimentCase>& cases,
    const ExecutionReport& execution,
    const std::vector<CaseSummary>& summaries,
    const std::filesystem::path& output_directory,
    std::chrono::system_clock::time_point completed_at,
    std::string_view revision = {});

}  // namespace airside::experiment
