#pragma once

#include "airside/scenario/scenario_loader.hpp"

#include <filesystem>

namespace airside::test {

inline Scenario baseline_scenario() {
    return load_scenario(std::filesystem::path{AIRSIDE_SOURCE_DIR} / "scenarios" / "baseline.yaml");
}

inline Scenario high_capacity_scenario() {
    return load_scenario(
        std::filesystem::path{AIRSIDE_SOURCE_DIR} / "scenarios" / "high_capacity.yaml");
}

}  // namespace airside::test
