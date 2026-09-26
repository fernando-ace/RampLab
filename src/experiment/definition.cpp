#include "airside/experiment/definition.hpp"

#include <algorithm>
#include <format>
#include <stdexcept>
#include <set>
#include <thread>
#include <unordered_set>

namespace airside::experiment {

std::string_view parameter_name(ParameterKey key) noexcept {
    switch (key) {
    case ParameterKey::FuelTruckCount: return "fleet.fuel_trucks";
    case ParameterKey::BaggageCartCount: return "fleet.baggage_carts";
    case ParameterKey::FuelServiceDuration: return "service_durations_seconds.fueling";
    case ParameterKey::BaggageServiceDuration: return "service_durations_seconds.baggage";
    case ParameterKey::FuelVehicleSpeed: return "vehicle_speed_mps.fueling";
    case ParameterKey::BaggageVehicleSpeed: return "vehicle_speed_mps.baggage";
    case ParameterKey::AircraftArrivalOffset: return "aircraft_schedule.arrival_offset_seconds";
    case ParameterKey::AircraftDepartureOffset: return "aircraft_schedule.departure_offset_seconds";
    case ParameterKey::RoadClosureEnabled: return "disruptions.road_closure.enabled";
    case ParameterKey::RoadClosureTime: return "disruptions.road_closure.time_seconds";
    }
    return "unknown";
}

std::string format_parameter_value(const ParameterValue& value) {
    return std::visit([](const auto& item) -> std::string {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::same_as<T, bool>) return item ? "true" : "false";
        else if constexpr (std::same_as<T, double>) return std::format("{:.6g}", item);
        else return std::to_string(item);
    }, value);
}

std::size_t automatic_worker_count() noexcept {
    const auto available = static_cast<std::size_t>(std::thread::hardware_concurrency());
    if (available <= 2) return 1;
    return available - 1;
}

void validate_definition(const ExperimentDefinition& definition) {
    if (definition.name.empty()) throw std::invalid_argument("experiment name cannot be empty");
    if (definition.scenario_path.empty()) throw std::invalid_argument("experiment scenario path cannot be empty");
    if (definition.seeds.values.empty()) throw std::invalid_argument("experiment requires at least one seed");
    if (definition.seeds.replications == 0) throw std::invalid_argument("replications must be positive");
    if (!definition.workers.automatic && definition.workers.count == 0) {
        throw std::invalid_argument("worker count must be positive");
    }
    std::unordered_set<std::uint64_t> seeds;
    for (const auto seed : definition.seeds.values) {
        if (!seeds.insert(seed).second) throw std::invalid_argument("seed values must be unique");
    }
    std::unordered_set<ParameterKey> keys;
    for (const auto& axis : definition.parameters) {
        if (axis.values.empty()) {
            throw std::invalid_argument(std::format("parameter '{}' requires at least one value", parameter_name(axis.key)));
        }
        if (!keys.insert(axis.key).second) {
            throw std::invalid_argument(std::format("duplicate parameter '{}'", parameter_name(axis.key)));
        }
        std::set<std::pair<std::size_t, std::string>> values;
        for (const auto& value : axis.values) {
            const auto identity = std::pair{value.index(), format_parameter_value(value)};
            if (!values.insert(identity).second) {
                throw std::invalid_argument(std::format("parameter '{}' contains duplicate values", parameter_name(axis.key)));
            }
        }
    }
}

}  // namespace airside::experiment
