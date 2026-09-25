#pragma once

#include "airside/experiment/definition.hpp"

#include <filesystem>
#include <stdexcept>

namespace airside::experiment {

class ExperimentLoadError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

[[nodiscard]] ExperimentDefinition load_experiment(const std::filesystem::path& path);

}  // namespace airside::experiment
