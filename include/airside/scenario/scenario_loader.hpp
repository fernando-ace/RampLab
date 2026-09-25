#pragma once

#include "airside/operations/simulation.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>

namespace airside {

class ScenarioLoadError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

[[nodiscard]] Scenario load_scenario(const std::filesystem::path& path);
[[nodiscard]] std::uint64_t resolve_seed(
    const Scenario& scenario,
    std::optional<std::uint64_t> command_line_seed) noexcept;

}  // namespace airside
