#include "airside/experiment/case_generator.hpp"

#include <algorithm>
#include <format>
#include <limits>
#include <stdexcept>

namespace airside::experiment {
namespace {

template <typename T>
T require_value(const ParameterValue& value, ParameterKey key) {
    const auto* result = std::get_if<T>(&value);
    if (result == nullptr) {
        throw std::invalid_argument(std::format("parameter '{}' has the wrong value type", parameter_name(key)));
    }
    return *result;
}

void apply_value(ScenarioOverrides& overrides, ParameterKey key, const ParameterValue& value) {
    const auto positive_count = [&] {
        const auto raw = require_value<std::int64_t>(value, key);
        if (raw <= 0) throw std::invalid_argument(std::format("parameter '{}' must be positive", parameter_name(key)));
        return static_cast<std::size_t>(raw);
    };
    const auto positive_duration = [&] {
        const auto raw = require_value<std::int64_t>(value, key);
        if (raw <= 0) throw std::invalid_argument(std::format("parameter '{}' must be positive", parameter_name(key)));
        return SimTime{raw};
    };
    const auto nonnegative_time = [&] {
        const auto raw = require_value<std::int64_t>(value, key);
        if (raw < 0) throw std::invalid_argument(std::format("parameter '{}' cannot be negative", parameter_name(key)));
        return SimTime{raw};
    };
    switch (key) {
    case ParameterKey::FuelTruckCount: overrides.fuel_truck_count = positive_count(); break;
    case ParameterKey::BaggageCartCount: overrides.baggage_cart_count = positive_count(); break;
    case ParameterKey::FuelServiceDuration: overrides.fuel_service_duration = positive_duration(); break;
    case ParameterKey::BaggageServiceDuration: overrides.baggage_service_duration = positive_duration(); break;
    case ParameterKey::FuelVehicleSpeed:
        overrides.fuel_vehicle_speed_mps = require_value<double>(value, key);
        if (*overrides.fuel_vehicle_speed_mps <= 0.0) throw std::invalid_argument("vehicle speed must be positive");
        break;
    case ParameterKey::BaggageVehicleSpeed:
        overrides.baggage_vehicle_speed_mps = require_value<double>(value, key);
        if (*overrides.baggage_vehicle_speed_mps <= 0.0) throw std::invalid_argument("vehicle speed must be positive");
        break;
    case ParameterKey::AircraftArrivalOffset:
        overrides.aircraft_arrival_offset = SimTime{require_value<std::int64_t>(value, key)}; break;
    case ParameterKey::AircraftDepartureOffset:
        overrides.aircraft_departure_offset = SimTime{require_value<std::int64_t>(value, key)}; break;
    case ParameterKey::RoadClosureEnabled:
        overrides.road_closure_enabled = require_value<bool>(value, key); break;
    case ParameterKey::RoadClosureTime: overrides.road_closure_time = nonnegative_time(); break;
    }
}

void build_cases_recursive(
    const ExperimentDefinition& definition,
    std::size_t axis_index,
    ExperimentCase current,
    std::vector<ExperimentCase>& output) {
    if (axis_index == definition.parameters.size()) {
        current.ordinal = output.size();
        current.id = std::format("case_{:04}", current.ordinal + 1);
        output.push_back(std::move(current));
        return;
    }
    const auto& axis = definition.parameters[axis_index];
    for (const auto& value : axis.values) {
        auto next = current;
        next.parameters.emplace_back(axis.key, value);
        apply_value(next.overrides, axis.key, value);
        build_cases_recursive(definition, axis_index + 1, std::move(next), output);
    }
}

std::vector<ServiceVehicle> rebuild_fleet(const Scenario& base, const ScenarioOverrides& overrides) {
    std::vector<ServiceVehicle> result;
    const auto add_type = [&](ServiceType type, std::optional<std::size_t> requested_count,
                              std::optional<double> requested_speed) {
        std::vector<const ServiceVehicle*> templates;
        for (const auto& vehicle : base.vehicles) {
            if (vehicle.capability() == type) templates.push_back(&vehicle);
        }
        if (templates.empty()) throw std::invalid_argument("base scenario is missing a required vehicle type");
        const auto count = requested_count.value_or(templates.size());
        for (std::size_t index = 0; index < count; ++index) {
            const auto& source = *templates[index % templates.size()];
            const auto label = type == ServiceType::Fueling ? "FuelTruck" : "BaggageCart";
            result.emplace_back(VehicleId{static_cast<std::uint32_t>(result.size() + 1)},
                std::format("{}-{}", label, index + 1), type, source.depot_node(),
                requested_speed.value_or(source.speed_mps()));
        }
    };
    add_type(ServiceType::Fueling, overrides.fuel_truck_count, overrides.fuel_vehicle_speed_mps);
    add_type(ServiceType::Baggage, overrides.baggage_cart_count, overrides.baggage_vehicle_speed_mps);
    return result;
}

std::vector<Aircraft> rebuild_aircraft(const Scenario& base, const ScenarioOverrides& overrides) {
    std::vector<Aircraft> result;
    result.reserve(base.aircraft.size());
    for (const auto& source : base.aircraft) {
        const auto arrival = source.scheduled_arrival() + overrides.aircraft_arrival_offset.value_or(SimTime::zero());
        const auto departure = source.scheduled_departure() + overrides.aircraft_departure_offset.value_or(SimTime::zero());
        if (arrival < SimTime::zero() || departure < arrival) {
            throw std::invalid_argument(std::format("schedule overrides make aircraft '{}' timestamps invalid", source.flight_number()));
        }
        std::vector<ServiceTask> tasks;
        tasks.reserve(source.tasks().size());
        for (const auto& task : source.tasks()) tasks.emplace_back(task.id, task.type);
        result.emplace_back(source.id(), source.flight_number(), arrival, departure,
            source.gate(), source.gate_node(), std::move(tasks));
    }
    return result;
}

}  // namespace

std::vector<ExperimentCase> generate_cases(const ExperimentDefinition& definition) {
    validate_definition(definition);
    std::vector<ExperimentCase> result;
    build_cases_recursive(definition, 0, {}, result);
    return result;
}

std::vector<RunRequest> generate_runs(
    const ExperimentDefinition& definition,
    const std::vector<ExperimentCase>& cases) {
    std::vector<RunRequest> result;
    result.reserve(cases.size() * definition.seeds.values.size() * definition.seeds.replications);
    for (const auto& experiment_case : cases) {
        for (const auto seed : definition.seeds.values) {
            for (std::size_t replication = 1; replication <= definition.seeds.replications; ++replication) {
                result.push_back({result.size(), experiment_case, seed, replication});
            }
        }
    }
    return result;
}

Scenario apply_overrides(const Scenario& base, const ScenarioOverrides& overrides) {
    Scenario result = base;
    result.vehicles = rebuild_fleet(base, overrides);
    result.aircraft = rebuild_aircraft(base, overrides);
    if (overrides.fuel_service_duration) {
        result.service_durations[ServiceType::Fueling] = *overrides.fuel_service_duration;
    }
    if (overrides.baggage_service_duration) {
        result.service_durations[ServiceType::Baggage] = *overrides.baggage_service_duration;
    }
    if (overrides.road_closure_time) {
        bool changed = false;
        for (auto& event : result.road_events) {
            if (!event.available) { event.time = *overrides.road_closure_time; changed = true; }
        }
        if (!changed) throw std::invalid_argument("road closure time override requires a closure in the base scenario");
    }
    if (overrides.road_closure_enabled == false) {
        std::erase_if(result.road_events, [](const RoadAvailabilityEvent& event) { return !event.available; });
    }
    if (overrides.road_closure_enabled == true &&
        std::ranges::none_of(result.road_events, [](const auto& event) { return !event.available; })) {
        throw std::invalid_argument("road closure enabled override requires a closure in the base scenario");
    }
    return result;
}

}  // namespace airside::experiment
