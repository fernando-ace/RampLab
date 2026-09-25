#pragma once

#include "airside/operations/simulation.hpp"

namespace airside {

[[nodiscard]] Scenario make_baseline_scenario(bool include_road_disruption = true);

}  // namespace airside
