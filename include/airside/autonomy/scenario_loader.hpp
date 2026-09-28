#pragma once

#include "airside/autonomy/simulation.hpp"

#include <filesystem>

namespace airside::autonomy {
[[nodiscard]] AutonomyScenario load_scenario(const std::filesystem::path& path);
}
