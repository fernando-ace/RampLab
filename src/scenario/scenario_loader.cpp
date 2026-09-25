#include "airside/scenario/scenario_loader.hpp"

#include "airside/routing/astar.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cstdint>
#include <format>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace airside {
namespace {

struct NodeDocument { std::string id; std::string name; double x; double y; };
struct EdgeDocument {
    std::string id; std::string from; std::string to; double distance;
    std::int64_t traversal_seconds; bool enabled;
};
struct GateDocument { std::string id; std::string name; std::string node; bool enabled; };
struct VehicleDocument {
    std::string id; std::string name; std::string type; std::string depot; double speed;
};
struct AircraftDocument {
    std::string id; std::string gate; std::int64_t arrival;
    std::int64_t departure; std::vector<std::string> services;
};
struct RoadEventDocument { std::int64_t time; std::string edge; bool enabled; };
struct ScenarioDocument {
    std::string name;
    std::uint64_t default_seed;
    std::vector<NodeDocument> nodes;
    std::vector<EdgeDocument> edges;
    std::vector<GateDocument> gates;
    std::vector<VehicleDocument> vehicles;
    std::vector<AircraftDocument> aircraft;
    std::int64_t fueling_seconds;
    std::int64_t baggage_seconds;
    std::vector<RoadEventDocument> road_events;
};

YAML::Node required(const YAML::Node& parent, const char* key, std::string_view context) {
    const auto value = parent[key];
    if (!value) throw ScenarioLoadError(std::format("{}: missing '{}'", context, key));
    return value;
}

template <typename T>
T scalar(const YAML::Node& parent, const char* key, std::string_view context) {
    try {
        return required(parent, key, context).as<T>();
    } catch (const YAML::Exception& error) {
        throw ScenarioLoadError(std::format("{}: invalid '{}': {}", context, key, error.what()));
    }
}

bool optional_bool(const YAML::Node& parent, const char* key, bool fallback, std::string_view context) {
    if (!parent[key]) return fallback;
    return scalar<bool>(parent, key, context);
}

void require_sequence(const YAML::Node& node, std::string_view context) {
    if (!node.IsSequence() || node.size() == 0) {
        throw ScenarioLoadError(std::format("{} must be a non-empty sequence", context));
    }
}

ScenarioDocument parse_document(const YAML::Node& root) {
    if (!root.IsMap()) throw ScenarioLoadError("scenario root must be a mapping");
    ScenarioDocument document;
    document.name = scalar<std::string>(root, "name", "scenario");
    document.default_seed = root["default_seed"] ?
        scalar<std::uint64_t>(root, "default_seed", "scenario") : 42U;

    const auto airport = required(root, "airport", "scenario");
    const auto nodes = required(airport, "nodes", "airport");
    const auto edges = required(airport, "edges", "airport");
    require_sequence(nodes, "airport.nodes");
    require_sequence(edges, "airport.edges");
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        const auto item = nodes[index];
        const auto context = std::format("airport.nodes[{}]", index);
        document.nodes.push_back({scalar<std::string>(item, "id", context),
            item["name"] ? scalar<std::string>(item, "name", context) : scalar<std::string>(item, "id", context),
            scalar<double>(item, "x_m", context), scalar<double>(item, "y_m", context)});
    }
    for (std::size_t index = 0; index < edges.size(); ++index) {
        const auto item = edges[index];
        const auto context = std::format("airport.edges[{}]", index);
        document.edges.push_back({scalar<std::string>(item, "id", context),
            scalar<std::string>(item, "from", context), scalar<std::string>(item, "to", context),
            scalar<double>(item, "distance_m", context),
            scalar<std::int64_t>(item, "traversal_time_seconds", context),
            optional_bool(item, "enabled", true, context)});
    }

    const auto gates = required(root, "gates", "scenario");
    require_sequence(gates, "gates");
    for (std::size_t index = 0; index < gates.size(); ++index) {
        const auto item = gates[index];
        const auto context = std::format("gates[{}]", index);
        document.gates.push_back({scalar<std::string>(item, "id", context),
            item["name"] ? scalar<std::string>(item, "name", context) : scalar<std::string>(item, "id", context),
            scalar<std::string>(item, "node", context), optional_bool(item, "enabled", true, context)});
    }

    const auto vehicles = required(required(root, "fleet", "scenario"), "vehicles", "fleet");
    require_sequence(vehicles, "fleet.vehicles");
    for (std::size_t index = 0; index < vehicles.size(); ++index) {
        const auto item = vehicles[index];
        const auto context = std::format("fleet.vehicles[{}]", index);
        document.vehicles.push_back({scalar<std::string>(item, "id", context),
            scalar<std::string>(item, "name", context), scalar<std::string>(item, "type", context),
            scalar<std::string>(item, "depot_node", context), scalar<double>(item, "speed_mps", context)});
    }

    const auto aircraft = required(root, "aircraft", "scenario");
    require_sequence(aircraft, "aircraft");
    for (std::size_t index = 0; index < aircraft.size(); ++index) {
        const auto item = aircraft[index];
        const auto context = std::format("aircraft[{}]", index);
        const auto services = required(item, "required_services", context);
        require_sequence(services, std::format("{}.required_services", context));
        AircraftDocument flight{scalar<std::string>(item, "id", context),
            scalar<std::string>(item, "gate", context),
            scalar<std::int64_t>(item, "scheduled_arrival_seconds", context),
            scalar<std::int64_t>(item, "scheduled_departure_seconds", context), {}};
        for (const auto service : services) flight.services.push_back(service.as<std::string>());
        document.aircraft.push_back(std::move(flight));
    }

    const auto durations = required(root, "service_durations_seconds", "scenario");
    document.fueling_seconds = scalar<std::int64_t>(durations, "fueling", "service_durations_seconds");
    document.baggage_seconds = scalar<std::int64_t>(durations, "baggage", "service_durations_seconds");

    if (const auto events = root["road_events"]) {
        if (!events.IsSequence()) throw ScenarioLoadError("road_events must be a sequence");
        for (std::size_t index = 0; index < events.size(); ++index) {
            const auto item = events[index];
            const auto context = std::format("road_events[{}]", index);
            document.road_events.push_back({scalar<std::int64_t>(item, "time_seconds", context),
                scalar<std::string>(item, "edge", context),
                scalar<bool>(item, "enabled", context)});
        }
    }
    return document;
}

template <typename Range, typename Projection>
void validate_unique(const Range& range, Projection projection, std::string_view category) {
    std::unordered_set<std::string> ids;
    for (const auto& value : range) {
        const auto& id = std::invoke(projection, value);
        if (id.empty()) throw ScenarioLoadError(std::format("{} ID cannot be empty", category));
        if (!ids.insert(id).second) {
            throw ScenarioLoadError(std::format("duplicate {} ID '{}'", category, id));
        }
    }
}

ServiceType parse_service(std::string_view value, std::string_view context) {
    if (value == "fueling") return ServiceType::Fueling;
    if (value == "baggage") return ServiceType::Baggage;
    throw ScenarioLoadError(std::format("{}: unknown service type '{}'", context, value));
}

template <typename Map>
auto lookup(const Map& map, const std::string& key, std::string_view context) -> typename Map::mapped_type {
    const auto found = map.find(key);
    if (found == map.end()) throw ScenarioLoadError(std::format("{} references missing ID '{}'", context, key));
    return found->second;
}

Scenario validate_and_build(const ScenarioDocument& document) {
    if (document.name.empty()) throw ScenarioLoadError("scenario name cannot be empty");
    validate_unique(document.nodes, &NodeDocument::id, "node");
    validate_unique(document.edges, &EdgeDocument::id, "edge");
    validate_unique(document.gates, &GateDocument::id, "gate");
    validate_unique(document.vehicles, &VehicleDocument::id, "vehicle");
    validate_unique(document.aircraft, &AircraftDocument::id, "aircraft");
    if (document.fueling_seconds <= 0 || document.baggage_seconds <= 0) {
        throw ScenarioLoadError("service durations must be positive");
    }

    Scenario scenario;
    scenario.name = document.name;
    scenario.default_seed = document.default_seed;
    std::unordered_map<std::string, NodeId> nodes;
    std::unordered_map<std::string, EdgeId> edges;
    std::unordered_map<std::string, GateId> gates;
    for (std::size_t index = 0; index < document.nodes.size(); ++index) {
        const auto id = NodeId{static_cast<std::uint32_t>(index + 1)};
        nodes.emplace(document.nodes[index].id, id);
        scenario.graph.add_node({id, document.nodes[index].name,
            {document.nodes[index].x, document.nodes[index].y}});
    }
    for (std::size_t index = 0; index < document.edges.size(); ++index) {
        const auto& value = document.edges[index];
        if (value.from == value.to || value.distance <= 0.0 || value.traversal_seconds <= 0) {
            throw ScenarioLoadError(std::format("edge '{}' has invalid geometry or traversal time", value.id));
        }
        const auto id = EdgeId{static_cast<std::uint32_t>(index + 1)};
        edges.emplace(value.id, id);
        scenario.graph.add_edge({id, lookup(nodes, value.from, std::format("edge '{}'.from", value.id)),
            lookup(nodes, value.to, std::format("edge '{}'.to", value.id)), value.distance,
            SimTime{value.traversal_seconds}, value.enabled});
    }
    for (std::size_t index = 0; index < document.gates.size(); ++index) {
        const auto& value = document.gates[index];
        const auto id = GateId{static_cast<std::uint32_t>(index + 1)};
        gates.emplace(value.id, id);
        scenario.gates.emplace_back(id, value.name,
            lookup(nodes, value.node, std::format("gate '{}'.node", value.id)), value.enabled);
    }

    bool has_fuel = false;
    bool has_baggage = false;
    for (std::size_t index = 0; index < document.vehicles.size(); ++index) {
        const auto& value = document.vehicles[index];
        if (value.speed <= 0.0) {
            throw ScenarioLoadError(std::format("vehicle '{}' speed_mps must be positive", value.id));
        }
        const auto type = parse_service(value.type, std::format("vehicle '{}'", value.id));
        has_fuel = has_fuel || type == ServiceType::Fueling;
        has_baggage = has_baggage || type == ServiceType::Baggage;
        scenario.vehicles.emplace_back(VehicleId{static_cast<std::uint32_t>(index + 1)}, value.name,
            type, lookup(nodes, value.depot, std::format("vehicle '{}'.depot_node", value.id)), value.speed);
    }
    if (!has_fuel || !has_baggage) {
        throw ScenarioLoadError("fleet requires at least one fueling and one baggage vehicle");
    }

    std::uint32_t next_task = 1;
    for (std::size_t index = 0; index < document.aircraft.size(); ++index) {
        const auto& value = document.aircraft[index];
        if (value.arrival < 0 || value.departure < 0 || value.departure < value.arrival) {
            throw ScenarioLoadError(std::format("aircraft '{}' has invalid timestamps", value.id));
        }
        const auto gate_id = lookup(gates, value.gate, std::format("aircraft '{}'.gate", value.id));
        const auto gate_it = std::ranges::find_if(scenario.gates,
            [&](const auto& gate) { return gate.id == gate_id; });
        std::vector<ServiceTask> tasks;
        std::unordered_set<ServiceType> unique_services;
        for (const auto& service : value.services) {
            const auto type = parse_service(service, std::format("aircraft '{}'", value.id));
            if (!unique_services.insert(type).second) {
                throw ScenarioLoadError(std::format("aircraft '{}' repeats a required service", value.id));
            }
            tasks.emplace_back(TaskId{next_task++}, type);
        }
        scenario.aircraft.emplace_back(AircraftId{static_cast<std::uint32_t>(index + 1)}, value.id,
            SimTime{value.arrival}, SimTime{value.departure}, gate_id, gate_it->node, std::move(tasks));
    }
    scenario.service_durations.emplace(ServiceType::Fueling, SimTime{document.fueling_seconds});
    scenario.service_durations.emplace(ServiceType::Baggage, SimTime{document.baggage_seconds});
    for (const auto& value : document.road_events) {
        if (value.time < 0) throw ScenarioLoadError("road event timestamp cannot be negative");
        scenario.road_events.push_back({SimTime{value.time},
            lookup(edges, value.edge, "road event edge"), value.enabled});
    }

    for (const auto& flight : scenario.aircraft) {
        for (const auto& task : flight.tasks()) {
            const auto reachable = std::ranges::any_of(scenario.vehicles, [&](const auto& vehicle) {
                return vehicle.capability() == task.type &&
                    find_route(scenario.graph, vehicle.depot_node(), flight.gate_node()).has_value();
            });
            if (!reachable) {
                throw ScenarioLoadError(std::format("aircraft '{}' gate has no route for required {} service",
                    flight.flight_number(), to_string(task.type)));
            }
        }
    }
    return scenario;
}

}  // namespace

Scenario load_scenario(const std::filesystem::path& path) {
    if (path.empty()) throw ScenarioLoadError("scenario path cannot be empty");
    try {
        return validate_and_build(parse_document(YAML::LoadFile(path.string())));
    } catch (const ScenarioLoadError&) {
        throw;
    } catch (const YAML::Exception& error) {
        throw ScenarioLoadError(std::format("failed to parse scenario '{}': {}", path.string(), error.what()));
    }
}

std::uint64_t resolve_seed(
    const Scenario& scenario,
    std::optional<std::uint64_t> command_line_seed) noexcept {
    return command_line_seed.value_or(scenario.default_seed);
}

}  // namespace airside
