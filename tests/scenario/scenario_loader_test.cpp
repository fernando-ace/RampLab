#include "airside/scenario/scenario_loader.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace airside {
namespace {

constexpr std::string_view kValidScenario = R"yaml(
name: fixture
default_seed: 7
airport:
  nodes:
    - { id: depot, x_m: 0, y_m: 0 }
    - { id: gate, x_m: 100, y_m: 0 }
  edges:
    - { id: road, from: depot, to: gate, distance_m: 100, traversal_time_seconds: 10 }
gates:
  - { id: A1, node: gate }
fleet:
  vehicles:
    - { id: fuel, name: Fuel-1, type: fueling, depot_node: depot, speed_mps: 10 }
    - { id: bag, name: Bag-1, type: baggage, depot_node: depot, speed_mps: 8 }
aircraft:
  - id: AX1
    gate: A1
    scheduled_arrival_seconds: 0
    scheduled_departure_seconds: 1200
    required_services: [fueling, baggage]
service_durations_seconds: { fueling: 480, baggage: 600 }
road_events:
  - { time_seconds: 100, edge: road, enabled: false }
)yaml";

class TemporaryScenario {
public:
    explicit TemporaryScenario(std::string contents) {
        static unsigned counter = 0;
        path_ = std::filesystem::temp_directory_path() /
            ("airside-scenario-test-" + std::to_string(++counter) + ".yaml");
        std::ofstream output(path_);
        output << contents;
    }
    ~TemporaryScenario() {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }
    const std::filesystem::path& path() const noexcept { return path_; }
private:
    std::filesystem::path path_;
};

std::string replaced(std::string text, std::string_view from, std::string_view to) {
    const auto position = text.find(from);
    EXPECT_NE(position, std::string::npos);
    if (position != std::string::npos) text.replace(position, from.size(), to);
    return text;
}

void load_and_discard(const std::filesystem::path& path) {
    [[maybe_unused]] const auto scenario = load_scenario(path);
}

TEST(ScenarioLoaderTest, LoadsExternalBaselineAndDefaultSeed) {
    const auto scenario = load_scenario(
        std::filesystem::path{AIRSIDE_SOURCE_DIR} / "scenarios" / "baseline.yaml");
    EXPECT_EQ(scenario.name, "baseline");
    EXPECT_EQ(scenario.default_seed, 42U);
    EXPECT_EQ(scenario.aircraft.size(), 3U);
    EXPECT_EQ(scenario.gates.size(), 3U);
    EXPECT_EQ(scenario.vehicles.size(), 2U);
    EXPECT_EQ(scenario.road_events.size(), 1U);
}

TEST(ScenarioLoaderTest, RejectsDuplicateIds) {
    const TemporaryScenario file{replaced(std::string{kValidScenario},
        "- { id: gate, x_m", "- { id: depot, x_m")};
    EXPECT_THROW(load_and_discard(file.path()), ScenarioLoadError);
}

TEST(ScenarioLoaderTest, RejectsMissingReferencedNode) {
    const TemporaryScenario file{replaced(std::string{kValidScenario}, "to: gate", "to: missing")};
    EXPECT_THROW(load_and_discard(file.path()), ScenarioLoadError);
}

TEST(ScenarioLoaderTest, RejectsAircraftReferencingNonexistentGate) {
    const TemporaryScenario file{replaced(std::string{kValidScenario}, "gate: A1", "gate: A9")};
    EXPECT_THROW(load_and_discard(file.path()), ScenarioLoadError);
}

TEST(ScenarioLoaderTest, RejectsInvalidEdgeAndDuration) {
    const TemporaryScenario edge_file{replaced(std::string{kValidScenario},
        "distance_m: 100", "distance_m: -1")};
    EXPECT_THROW(load_and_discard(edge_file.path()), ScenarioLoadError);
    const TemporaryScenario duration_file{replaced(std::string{kValidScenario},
        "fueling: 480", "fueling: -1")};
    EXPECT_THROW(load_and_discard(duration_file.path()), ScenarioLoadError);
}

TEST(ScenarioLoaderTest, RejectsInvalidVehicleSpeedAndRoadEvent) {
    const TemporaryScenario speed_file{replaced(std::string{kValidScenario},
        "speed_mps: 10", "speed_mps: 0")};
    EXPECT_THROW(load_and_discard(speed_file.path()), ScenarioLoadError);
    const TemporaryScenario event_file{replaced(std::string{kValidScenario},
        "edge: road", "edge: missing")};
    EXPECT_THROW(load_and_discard(event_file.path()), ScenarioLoadError);
}

TEST(ScenarioLoaderTest, RejectsMalformedYaml) {
    const TemporaryScenario file{"name: broken\nairport: [unterminated"};
    EXPECT_THROW(load_and_discard(file.path()), ScenarioLoadError);
}

TEST(ScenarioLoaderTest, CommandLineSeedOverridesScenarioDefault) {
    TemporaryScenario file{std::string{kValidScenario}};
    const auto scenario = load_scenario(file.path());
    EXPECT_EQ(resolve_seed(scenario, std::nullopt), 7U);
    EXPECT_EQ(resolve_seed(scenario, 99U), 99U);
}

}  // namespace
}  // namespace airside
