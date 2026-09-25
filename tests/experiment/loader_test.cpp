#include "airside/experiment/experiment_loader.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace airside::experiment {
namespace {

class ExperimentLoaderTest : public ::testing::Test {
protected:
    void SetUp() override {
        directory_ = std::filesystem::temp_directory_path() / "ramplab_experiment_loader_tests";
        std::filesystem::create_directories(directory_);
    }
    void TearDown() override { std::filesystem::remove_all(directory_); }

    std::filesystem::path write(std::string_view name, std::string_view contents) const {
        const auto path = directory_ / name;
        std::ofstream output{path};
        output << contents;
        return path;
    }

    std::string valid_yaml(std::string_view workers = "2", std::string_view seeds = "{ start: 5, count: 3 }") const {
        const auto scenario = (std::filesystem::path{AIRSIDE_SOURCE_DIR} / "scenarios" / "baseline.yaml").generic_string();
        return "name: loader_test\nscenario: \"" + scenario + "\"\nseeds: " + std::string{seeds} +
            "\nparameters:\n  fleet.fuel_trucks: [1, 2]\n  disruptions.road_closure.enabled: [true, false]\nworkers: " +
            std::string{workers} + "\noutputs:\n  directory: results\n";
    }

    std::filesystem::path directory_;
};

TEST_F(ExperimentLoaderTest, LoadsValidatedFileSeedRangeAndTypedParameters) {
    const auto path = write("valid.yaml", valid_yaml("auto"));
    const auto result = load_experiment(path);
    EXPECT_EQ(result.name, "loader_test");
    EXPECT_EQ(result.seeds.values, (std::vector<std::uint64_t>{5, 6, 7}));
    EXPECT_TRUE(result.workers.automatic);
    ASSERT_EQ(result.parameters.size(), 2U);
    EXPECT_EQ(result.parameters[0].key, ParameterKey::FuelTruckCount);
    EXPECT_EQ(result.parameters[1].key, ParameterKey::RoadClosureEnabled);
    EXPECT_EQ(result.output_directory, directory_ / "results");
}

TEST_F(ExperimentLoaderTest, SupportsSingleSeedListAndReplications) {
    auto single = load_experiment(write("single.yaml", valid_yaml("1", "42")));
    EXPECT_EQ(single.seeds.values, (std::vector<std::uint64_t>{42}));
    auto list = load_experiment(write("list.yaml", valid_yaml("1", "[2, 4, 8]")));
    EXPECT_EQ(list.seeds.values, (std::vector<std::uint64_t>{2, 4, 8}));
    auto replicated = load_experiment(write("replicated.yaml", valid_yaml("1", "{ values: [3, 9], replications: 2 }")));
    EXPECT_EQ(replicated.seeds.replications, 2U);
}

TEST_F(ExperimentLoaderTest, RejectsMissingScenarioInvalidWorkersAndSeedRange) {
    auto missing = valid_yaml();
    const auto scenario_start = missing.find("scenario: \"") + std::string{"scenario: \""}.size();
    const auto scenario_end = missing.find('"', scenario_start);
    missing.replace(scenario_start, scenario_end - scenario_start, "nonexistent-scenario.yaml");
    EXPECT_THROW({ [[maybe_unused]] const auto ignored = load_experiment(write("missing.yaml", missing)); }, ExperimentLoadError);
    EXPECT_THROW({ [[maybe_unused]] const auto ignored = load_experiment(write("workers.yaml", valid_yaml("0"))); }, ExperimentLoadError);
    EXPECT_THROW({ [[maybe_unused]] const auto ignored = load_experiment(write("seeds.yaml", valid_yaml("1", "{ start: 1, count: 0 }"))); }, ExperimentLoadError);
}

TEST_F(ExperimentLoaderTest, RejectsUnknownParameterAndMalformedYaml) {
    auto unknown = valid_yaml();
    const auto key = unknown.find("fleet.fuel_trucks");
    unknown.replace(key, std::string{"fleet.fuel_trucks"}.size(), "fleet.hovercraft");
    EXPECT_THROW({ [[maybe_unused]] const auto ignored = load_experiment(write("unknown.yaml", unknown)); }, ExperimentLoadError);
    EXPECT_THROW({ [[maybe_unused]] const auto ignored = load_experiment(write("malformed.yaml", "name: [unterminated\n")); }, ExperimentLoadError);
}

}  // namespace
}  // namespace airside::experiment
