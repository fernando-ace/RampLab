#pragma once

#include "airside/experiment/definition.hpp"
#include "airside/operations/simulation.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace airside::experiment {

struct ExperimentCase {
    std::size_t ordinal{0};
    std::string id;
    ScenarioOverrides overrides;
    std::vector<std::pair<ParameterKey, ParameterValue>> parameters;
};

struct RunRequest {
    std::size_t ordinal{0};
    ExperimentCase experiment_case;
    std::uint64_t seed{0};
    std::size_t replication{1};
};

[[nodiscard]] std::vector<ExperimentCase> generate_cases(const ExperimentDefinition& definition);
[[nodiscard]] std::vector<RunRequest> generate_runs(
    const ExperimentDefinition& definition,
    const std::vector<ExperimentCase>& cases);
[[nodiscard]] Scenario apply_overrides(const Scenario& base, const ScenarioOverrides& overrides);

}  // namespace airside::experiment
