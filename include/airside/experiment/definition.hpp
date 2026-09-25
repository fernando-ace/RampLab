#pragma once

#include "airside/core/types.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace airside::experiment {

enum class ParameterKey {
    FuelTruckCount,
    BaggageCartCount,
    FuelServiceDuration,
    BaggageServiceDuration,
    FuelVehicleSpeed,
    BaggageVehicleSpeed,
    AircraftArrivalOffset,
    AircraftDepartureOffset,
    RoadClosureEnabled,
    RoadClosureTime,
};

using ParameterValue = std::variant<std::int64_t, double, bool>;

struct ParameterAxis {
    ParameterKey key;
    std::vector<ParameterValue> values;
};

struct SeedDefinition {
    std::vector<std::uint64_t> values;
    std::size_t replications{1};
};

struct WorkerSetting {
    bool automatic{true};
    std::size_t count{0};
};

struct ExperimentDefinition {
    std::string name;
    std::filesystem::path source_file;
    std::filesystem::path scenario_path;
    SeedDefinition seeds;
    std::vector<ParameterAxis> parameters;
    WorkerSetting workers;
    std::filesystem::path output_directory;
};

struct ScenarioOverrides {
    std::optional<std::size_t> fuel_truck_count;
    std::optional<std::size_t> baggage_cart_count;
    std::optional<SimTime> fuel_service_duration;
    std::optional<SimTime> baggage_service_duration;
    std::optional<double> fuel_vehicle_speed_mps;
    std::optional<double> baggage_vehicle_speed_mps;
    std::optional<SimTime> aircraft_arrival_offset;
    std::optional<SimTime> aircraft_departure_offset;
    std::optional<bool> road_closure_enabled;
    std::optional<SimTime> road_closure_time;
};

[[nodiscard]] std::string_view parameter_name(ParameterKey key) noexcept;
[[nodiscard]] std::string format_parameter_value(const ParameterValue& value);
[[nodiscard]] std::size_t automatic_worker_count() noexcept;
void validate_definition(const ExperimentDefinition& definition);

}  // namespace airside::experiment
