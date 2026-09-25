#include "airside/experiment/experiment_loader.hpp"

#include <yaml-cpp/yaml.h>

#include <format>
#include <limits>
#include <unordered_map>

namespace airside::experiment {
namespace {

const std::unordered_map<std::string, ParameterKey> parameter_keys{
    {"fleet.fuel_trucks", ParameterKey::FuelTruckCount},
    {"fleet.baggage_carts", ParameterKey::BaggageCartCount},
    {"service_durations_seconds.fueling", ParameterKey::FuelServiceDuration},
    {"service_durations_seconds.baggage", ParameterKey::BaggageServiceDuration},
    {"vehicle_speed_mps.fueling", ParameterKey::FuelVehicleSpeed},
    {"vehicle_speed_mps.baggage", ParameterKey::BaggageVehicleSpeed},
    {"aircraft_schedule.arrival_offset_seconds", ParameterKey::AircraftArrivalOffset},
    {"aircraft_schedule.departure_offset_seconds", ParameterKey::AircraftDepartureOffset},
    {"disruptions.road_closure.enabled", ParameterKey::RoadClosureEnabled},
    {"disruptions.road_closure.time_seconds", ParameterKey::RoadClosureTime},
};

YAML::Node required(const YAML::Node& node, std::string_view key, std::string_view context) {
    const auto value = node[std::string{key}];
    if (!value) throw ExperimentLoadError(std::format("{} requires '{}'", context, key));
    return value;
}

template <typename T>
T scalar(const YAML::Node& node, std::string_view key, std::string_view context) {
    const auto value = required(node, key, context);
    if (!value.IsScalar()) throw ExperimentLoadError(std::format("{}.{} must be a scalar", context, key));
    try { return value.as<T>(); }
    catch (const YAML::Exception& error) {
        throw ExperimentLoadError(std::format("{}.{} has an invalid value: {}", context, key, error.what()));
    }
}

ParameterValue parse_parameter_value(ParameterKey key, const YAML::Node& node, std::string_view context) {
    if (!node.IsScalar()) throw ExperimentLoadError(std::format("{} values must be scalars", context));
    try {
        switch (key) {
        case ParameterKey::RoadClosureEnabled: return node.as<bool>();
        case ParameterKey::FuelVehicleSpeed:
        case ParameterKey::BaggageVehicleSpeed: return node.as<double>();
        default: return node.as<std::int64_t>();
        }
    } catch (const YAML::Exception& error) {
        throw ExperimentLoadError(std::format("{} has an invalid value: {}", context, error.what()));
    }
}

std::vector<std::uint64_t> parse_seeds(const YAML::Node& seeds, std::size_t& replications) {
    std::vector<std::uint64_t> result;
    if (seeds.IsScalar()) {
        result.push_back(seeds.as<std::uint64_t>());
    } else if (seeds.IsSequence()) {
        for (const auto& seed : seeds) result.push_back(seed.as<std::uint64_t>());
    } else if (seeds.IsMap()) {
        replications = seeds["replications"] ? scalar<std::size_t>(seeds, "replications", "seeds") : 1;
        if (seeds["values"]) {
            const auto values = seeds["values"];
            if (!values.IsSequence()) throw ExperimentLoadError("seeds.values must be a sequence");
            for (const auto& seed : values) result.push_back(seed.as<std::uint64_t>());
        } else {
            const auto start = scalar<std::uint64_t>(seeds, "start", "seeds");
            const auto count = scalar<std::size_t>(seeds, "count", "seeds");
            if (count == 0) throw ExperimentLoadError("seeds.count must be positive");
            if (start > std::numeric_limits<std::uint64_t>::max() - (count - 1)) {
                throw ExperimentLoadError("seed range overflows uint64");
            }
            result.reserve(count);
            for (std::size_t index = 0; index < count; ++index) result.push_back(start + index);
        }
    } else {
        throw ExperimentLoadError("seeds must be a scalar, sequence, or map");
    }
    return result;
}

std::filesystem::path resolve_path(
    const std::filesystem::path& experiment_file,
    const std::filesystem::path& configured) {
    if (configured.is_absolute()) return configured.lexically_normal();
    return (experiment_file.parent_path() / configured).lexically_normal();
}

}  // namespace

ExperimentDefinition load_experiment(const std::filesystem::path& path) {
    if (path.empty()) throw ExperimentLoadError("experiment path cannot be empty");
    try {
        const auto root = YAML::LoadFile(path.string());
        if (!root.IsMap()) throw ExperimentLoadError("experiment document must be a map");
        ExperimentDefinition definition;
        definition.source_file = std::filesystem::absolute(path).lexically_normal();
        definition.name = scalar<std::string>(root, "name", "experiment");
        definition.scenario_path = resolve_path(definition.source_file,
            scalar<std::string>(root, "scenario", "experiment"));
        definition.seeds.values = parse_seeds(required(root, "seeds", "experiment"), definition.seeds.replications);

        const auto parameters = required(root, "parameters", "experiment");
        if (!parameters.IsMap()) throw ExperimentLoadError("parameters must be a map");
        for (const auto& entry : parameters) {
            const auto name = entry.first.as<std::string>();
            const auto found = parameter_keys.find(name);
            if (found == parameter_keys.end()) throw ExperimentLoadError(std::format("unknown parameter '{}'", name));
            if (!entry.second.IsSequence() || entry.second.size() == 0) {
                throw ExperimentLoadError(std::format("parameter '{}' must be a non-empty sequence", name));
            }
            ParameterAxis axis{found->second, {}};
            for (const auto& value : entry.second) axis.values.push_back(parse_parameter_value(found->second, value, name));
            definition.parameters.push_back(std::move(axis));
        }

        const auto workers = required(root, "workers", "experiment");
        if (!workers.IsScalar()) throw ExperimentLoadError("workers must be 'auto' or a positive integer");
        const auto worker_text = workers.as<std::string>();
        if (worker_text == "auto") definition.workers = {true, 0};
        else {
            std::size_t consumed = 0;
            const auto count = std::stoull(worker_text, &consumed);
            if (consumed != worker_text.size()) throw ExperimentLoadError("workers must be 'auto' or a positive integer");
            definition.workers = {false, static_cast<std::size_t>(count)};
        }

        const auto outputs = required(root, "outputs", "experiment");
        definition.output_directory = resolve_path(definition.source_file,
            scalar<std::string>(outputs, "directory", "outputs"));
        validate_definition(definition);
        if (!std::filesystem::is_regular_file(definition.scenario_path)) {
            throw ExperimentLoadError(std::format("scenario file does not exist: {}", definition.scenario_path.string()));
        }
        return definition;
    } catch (const ExperimentLoadError&) {
        throw;
    } catch (const YAML::Exception& error) {
        throw ExperimentLoadError(std::format("failed to parse experiment '{}': {}", path.string(), error.what()));
    } catch (const std::exception& error) {
        throw ExperimentLoadError(std::format("invalid experiment '{}': {}", path.string(), error.what()));
    }
}

}  // namespace airside::experiment
