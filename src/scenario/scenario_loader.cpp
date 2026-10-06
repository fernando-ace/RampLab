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
    std::int64_t traversal_seconds; bool enabled; bool one_way;
};
struct GateDocument { std::string id; std::string name; std::string node; bool enabled; };
struct VehicleDocument {
    std::string id; std::string name; std::string type; std::string depot; double speed;
    std::optional<std::string> outage_safe_node;
};
struct AircraftDocument {
    std::string id; std::string gate; std::int64_t arrival;
    std::int64_t departure; std::vector<std::string> services;
    std::string turnaround_id; std::int64_t target_off_block;
    bool arrival_only{false}; std::string arrival_exit;
    struct Task { std::string id; std::string type; std::vector<std::string> prerequisites;
        std::int64_t earliest_start{0}; std::optional<std::int64_t> latest_completion;
        std::optional<std::int64_t> duration; std::string resource; };
    std::vector<Task> tasks;
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
    std::unordered_map<std::string, std::int64_t> service_durations;
    std::vector<RoadEventDocument> road_events;
    struct Disruption { std::int64_t time; std::string aircraft; std::string task; std::int64_t duration; };
    std::vector<Disruption> disruptions;
    struct VehicleOutage { std::int64_t time; std::string vehicle; };
    std::vector<VehicleOutage> vehicle_outages;
    struct SurfaceOperations { std::string departure_handoff; std::string runway_node; std::string arrival_exit; std::int64_t pushback_seconds{30}; std::int64_t runway_seconds{60}; std::int64_t arrival_rollout_seconds{90}; double speed_mps{5.0}; double queue_spacing_m{30.0}; };
    std::optional<SurfaceOperations> surface_operations;
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
            optional_bool(item, "enabled", true, context), optional_bool(item, "one_way", false, context)});
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
            scalar<std::string>(item, "depot_node", context), scalar<double>(item, "speed_mps", context),
            item["outage_safe_node"] ? std::optional<std::string>{scalar<std::string>(item, "outage_safe_node", context)} : std::nullopt});
    }

    const auto aircraft = required(root, "aircraft", "scenario");
    require_sequence(aircraft, "aircraft");
    for (std::size_t index = 0; index < aircraft.size(); ++index) {
        const auto item = aircraft[index];
        const auto context = std::format("aircraft[{}]", index);
        const auto operation_type = item["operation_type"] ? scalar<std::string>(item, "operation_type", context) : "turnaround";
        if (operation_type != "turnaround" && operation_type != "arrival")
            throw ScenarioLoadError(std::format("{}: operation_type must be 'turnaround' or 'arrival'", context));
        const auto services = item["required_services"];
        const auto tasks = item["service_tasks"];
        if (operation_type == "turnaround" && !services && !tasks) throw ScenarioLoadError(std::format("{}: missing 'required_services' or 'service_tasks'", context));
        if (services) require_sequence(services, std::format("{}.required_services", context));
        if (tasks && !tasks.IsSequence()) throw ScenarioLoadError(std::format("{}.service_tasks must be a sequence", context));
        if (tasks && tasks.size() == 0) throw ScenarioLoadError(std::format("{}.service_tasks cannot be empty", context));
        AircraftDocument flight;
        flight.id = scalar<std::string>(item, "id", context);
        flight.arrival_only = operation_type == "arrival";
        if (flight.arrival_only) flight.arrival_exit = scalar<std::string>(item, "arrival_exit", context);
        flight.gate = scalar<std::string>(item, "gate", context);
        flight.arrival = scalar<std::int64_t>(item, "scheduled_arrival_seconds", context);
        flight.departure = item["scheduled_departure_seconds"] ? scalar<std::int64_t>(item, "scheduled_departure_seconds", context) : flight.arrival;
        flight.turnaround_id = item["turnaround_id"] ? scalar<std::string>(item, "turnaround_id", context) : flight.id;
        flight.target_off_block = item["target_off_block_seconds"] ? scalar<std::int64_t>(item, "target_off_block_seconds", context) : flight.departure;
        if (services) for (const auto service : services) flight.services.push_back(service.as<std::string>());
        if (tasks) for (std::size_t task_index = 0; task_index < tasks.size(); ++task_index) {
            const auto task = tasks[task_index];
            const auto task_context = std::format("{}.service_tasks[{}]", context, task_index);
            AircraftDocument::Task value;
            value.id = scalar<std::string>(task, "id", task_context);
            value.type = scalar<std::string>(task, "type", task_context);
            if (task["earliest_start_seconds"]) value.earliest_start = scalar<std::int64_t>(task, "earliest_start_seconds", task_context);
            if (task["latest_completion_seconds"]) value.latest_completion = scalar<std::int64_t>(task, "latest_completion_seconds", task_context);
            if (task["duration_seconds"]) value.duration = scalar<std::int64_t>(task, "duration_seconds", task_context);
            if (task["resource"]) value.resource = scalar<std::string>(task, "resource", task_context);
            if (task["prerequisites"]) {
                if (!task["prerequisites"].IsSequence()) throw ScenarioLoadError(std::format("{}.prerequisites must be a sequence", task_context));
                for (const auto prerequisite : task["prerequisites"]) value.prerequisites.push_back(prerequisite.as<std::string>());
            }
            flight.tasks.push_back(std::move(value));
        }
        document.aircraft.push_back(std::move(flight));
    }

    const auto durations = required(root, "service_durations_seconds", "scenario");
    for (const auto& name : {"fueling", "baggage", "deboarding", "catering", "cabin_cleaning", "baggage_load", "pushback_preparation"}) {
        if (durations[name]) document.service_durations.emplace(name, scalar<std::int64_t>(durations, name, "service_durations_seconds"));
    }
    if (!document.service_durations.contains("fueling") || !document.service_durations.contains("baggage")) {
        throw ScenarioLoadError("service_durations_seconds requires fueling and baggage durations");
    }

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
    if (const auto disruptions = root["turnaround_disruptions"]) {
        if (!disruptions.IsSequence()) throw ScenarioLoadError("turnaround_disruptions must be a sequence");
        for (std::size_t index = 0; index < disruptions.size(); ++index) {
            const auto item = disruptions[index];
            const auto context = std::format("turnaround_disruptions[{}]", index);
            document.disruptions.push_back({scalar<std::int64_t>(item, "time_seconds", context),
                scalar<std::string>(item, "aircraft", context), scalar<std::string>(item, "task", context),
                scalar<std::int64_t>(item, "duration_seconds", context)});
        }
    }
    if (const auto outages = root["vehicle_outages"]) {
        if (!outages.IsSequence()) throw ScenarioLoadError("vehicle_outages must be a sequence");
        for (std::size_t index = 0; index < outages.size(); ++index) {
            const auto item = outages[index];
            const auto context = std::format("vehicle_outages[{}]", index);
            document.vehicle_outages.push_back({scalar<std::int64_t>(item, "time_seconds", context),
                scalar<std::string>(item, "vehicle", context)});
        }
    }
    if (const auto surface = root["surface_operations"]) {
        ScenarioDocument::SurfaceOperations config;
        config.departure_handoff = scalar<std::string>(surface, "departure_handoff", "surface_operations");
        config.runway_node = surface["runway_node"] ? scalar<std::string>(surface, "runway_node", "surface_operations") : config.departure_handoff;
        config.arrival_exit = surface["arrival_exit"] ? scalar<std::string>(surface, "arrival_exit", "surface_operations") : config.departure_handoff;
        if (surface["pushback_seconds"]) config.pushback_seconds = scalar<std::int64_t>(surface, "pushback_seconds", "surface_operations");
        if (surface["runway_occupancy_seconds"]) config.runway_seconds = scalar<std::int64_t>(surface, "runway_occupancy_seconds", "surface_operations");
        if (surface["arrival_rollout_seconds"]) config.arrival_rollout_seconds = scalar<std::int64_t>(surface, "arrival_rollout_seconds", "surface_operations");
        if (surface["aircraft_speed_mps"]) config.speed_mps = scalar<double>(surface, "aircraft_speed_mps", "surface_operations");
        if (surface["departure_queue_spacing_m"]) config.queue_spacing_m = scalar<double>(surface, "departure_queue_spacing_m", "surface_operations");
        document.surface_operations = std::move(config);
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
    if (value == "deboarding") return ServiceType::Deboarding;
    if (value == "catering") return ServiceType::Catering;
    if (value == "cabin_cleaning") return ServiceType::CabinCleaning;
    if (value == "baggage_load") return ServiceType::BaggageLoad;
    if (value == "pushback_preparation") return ServiceType::PushbackPreparation;
    throw ScenarioLoadError(std::format("{}: unknown service type '{}'", context, value));
}

std::string service_key(ServiceType type) {
    switch (type) {
    case ServiceType::Fueling: return "fueling";
    case ServiceType::Baggage: return "baggage";
    case ServiceType::Deboarding: return "deboarding";
    case ServiceType::Catering: return "catering";
    case ServiceType::CabinCleaning: return "cabin_cleaning";
    case ServiceType::BaggageLoad: return "baggage_load";
    case ServiceType::PushbackPreparation: return "pushback_preparation";
    }
    return "";
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
    for (const auto& [name, seconds] : document.service_durations) {
        if (seconds <= 0) throw ScenarioLoadError(std::format("service duration '{}' must be positive", name));
    }

    Scenario scenario;
    scenario.name = document.name;
    scenario.default_seed = document.default_seed;
    scenario.turnaround_orchestration = !document.disruptions.empty() ||
        std::ranges::any_of(document.aircraft, [](const auto& flight) { return !flight.tasks.empty(); });
    std::unordered_map<std::string, NodeId> nodes;
    std::unordered_map<std::string, EdgeId> edges;
    std::unordered_map<std::string, GateId> gates;
    for (std::size_t index = 0; index < document.nodes.size(); ++index) {
        const auto id = NodeId{static_cast<std::uint32_t>(index + 1)};
        nodes.emplace(document.nodes[index].id, id);
        scenario.graph.add_node({id, document.nodes[index].name,
            {document.nodes[index].x, document.nodes[index].y}});
    }
    if (document.surface_operations) {
        const auto& config = *document.surface_operations;
        if (config.pushback_seconds <= 0 || config.runway_seconds <= 0 || config.arrival_rollout_seconds <= 0 ||
            config.speed_mps <= 0.0 || config.queue_spacing_m <= 0.0)
            throw ScenarioLoadError("surface_operations durations, aircraft_speed_mps, and departure_queue_spacing_m must be positive");
        scenario.surface_operations = SurfaceOperationsConfig{
            lookup(nodes, config.departure_handoff, "surface_operations.departure_handoff"),
            lookup(nodes, config.runway_node, "surface_operations.runway_node"),
            lookup(nodes, config.arrival_exit, "surface_operations.arrival_exit"),
            SimTime{config.pushback_seconds}, SimTime{config.runway_seconds},
            SimTime{config.arrival_rollout_seconds}, config.speed_mps, config.queue_spacing_m};
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
            SimTime{value.traversal_seconds}, value.enabled, value.one_way});
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
            type, lookup(nodes, value.depot, std::format("vehicle '{}'.depot_node", value.id)), value.speed,
            value.outage_safe_node ? std::optional<NodeId>{lookup(nodes, *value.outage_safe_node,
                std::format("vehicle '{}'.outage_safe_node", value.id))} : std::nullopt);
    }
    if (!has_fuel || !has_baggage) {
        throw ScenarioLoadError("fleet requires at least one fueling and one baggage vehicle");
    }

    std::uint32_t next_task = 1;
    std::unordered_map<std::string, TaskId> task_ids;
    std::unordered_map<std::string, std::unordered_map<std::string, TaskId>> aircraft_task_ids;
    std::unordered_set<std::string> turnaround_ids;
    for (std::size_t index = 0; index < document.aircraft.size(); ++index) {
        const auto& value = document.aircraft[index];
        if (value.arrival < 0 || value.departure < 0 || value.departure < value.arrival) {
            throw ScenarioLoadError(std::format("aircraft '{}' has invalid timestamps", value.id));
        }
        const auto gate_id = lookup(gates, value.gate, std::format("aircraft '{}'.gate", value.id));
        if (value.turnaround_id.empty() || !turnaround_ids.insert(value.turnaround_id).second) {
            throw ScenarioLoadError(std::format("aircraft '{}' has an empty or duplicate turnaround_id", value.id));
        }
        if (value.target_off_block < 0) throw ScenarioLoadError(std::format("aircraft '{}' has a negative target off-block time", value.id));
        const auto gate_it = std::ranges::find_if(scenario.gates,
            [&](const auto& gate) { return gate.id == gate_id; });
        std::vector<ServiceTask> tasks;
        std::unordered_set<ServiceType> unique_services;
        auto make_task = [&](const AircraftDocument::Task& source) {
            const auto type = parse_service(source.type, std::format("aircraft '{}' task '{}'", value.id, source.id));
            if (!unique_services.insert(type).second) {
                throw ScenarioLoadError(std::format("aircraft '{}' repeats a required service", value.id));
            }
            const auto global_id = TaskId{next_task++};
            if (source.id.empty()) throw ScenarioLoadError(std::format("aircraft '{}' task ID cannot be empty", value.id));
            if (!aircraft_task_ids[value.id].emplace(source.id, global_id).second) {
                throw ScenarioLoadError(std::format("aircraft '{}' repeats task ID '{}'", value.id, source.id));
            }
            task_ids.emplace(value.id + "/" + source.id, global_id);
            ServiceTask built{global_id, type};
            built.earliest_start = SimTime{source.earliest_start};
            if (source.latest_completion) built.latest_desirable_completion = SimTime{*source.latest_completion};
            auto duration = source.duration;
            const auto configured = document.service_durations.find(service_key(type));
            if (!duration && configured != document.service_durations.end()) duration = configured->second;
            if (!duration) duration = 300;
            if (*duration <= 0 || source.earliest_start < 0 || (source.latest_completion && *source.latest_completion < 0)) {
                throw ScenarioLoadError(std::format("aircraft '{}' task '{}' has invalid timing", value.id, source.id));
            }
            built.duration = SimTime{*duration};
            built.required_resource = source.resource;
            const auto expected_resource = type == ServiceType::Fueling ? "fuel_truck" :
                type == ServiceType::Baggage ? "baggage_vehicle" : "abstract_crew";
            if (!built.required_resource.empty() && built.required_resource != expected_resource) {
                throw ScenarioLoadError(std::format("aircraft '{}' task '{}' requires unsupported resource type '{}'",
                    value.id, source.id, built.required_resource));
            }
            if (built.required_resource.empty()) {
                built.required_resource = expected_resource;
            }
            tasks.push_back(std::move(built));
        };
        if (!value.tasks.empty()) {
            for (const auto& source : value.tasks) make_task(source);
        } else for (const auto& service : value.services) {
            make_task({service, service, {}, 0, std::nullopt, std::nullopt, {}});
        }
        if (value.arrival_only && !scenario.surface_operations)
            throw ScenarioLoadError(std::format("arrival aircraft '{}' requires surface_operations", value.id));
        scenario.aircraft.emplace_back(AircraftId{static_cast<std::uint32_t>(index + 1)}, value.id,
            SimTime{value.arrival}, SimTime{value.departure}, gate_id, gate_it->node, std::move(tasks),
            value.turnaround_id, SimTime{value.target_off_block},
            value.arrival_only ? AircraftOperationType::ArrivalOnly : AircraftOperationType::Turnaround,
            value.arrival_only ? lookup(nodes, value.arrival_exit, std::format("aircraft '{}'.arrival_exit", value.id)) : NodeId{});
    }
    for (const auto& [name, seconds] : document.service_durations) {
        const auto type = parse_service(name, "service_durations_seconds");
        scenario.service_durations.emplace(type, SimTime{seconds});
    }
    for (const auto& type : {ServiceType::Deboarding, ServiceType::Catering, ServiceType::CabinCleaning,
                             ServiceType::BaggageLoad, ServiceType::PushbackPreparation}) {
        if (!scenario.service_durations.contains(type)) scenario.service_durations.emplace(type, SimTime{300});
        scenario.abstract_resource_capacity.emplace(type, 1U);
    }
    // Resolve task references and reject cycles before the scenario reaches the core.
    for (std::size_t index = 0; index < document.aircraft.size(); ++index) {
        const auto& source = document.aircraft[index];
        auto& flight = scenario.aircraft[index];
        if (!source.tasks.empty()) {
            for (std::size_t task_index = 0; task_index < source.tasks.size(); ++task_index) {
                auto& target = flight.mutable_task(aircraft_task_ids[source.id].at(source.tasks[task_index].id));
                for (const auto& prerequisite : source.tasks[task_index].prerequisites) {
                    const auto found = aircraft_task_ids[source.id].find(prerequisite);
                    if (found == aircraft_task_ids[source.id].end()) {
                        throw ScenarioLoadError(std::format("aircraft '{}' task '{}' references missing prerequisite '{}'",
                            source.id, source.tasks[task_index].id, prerequisite));
                    }
                    if (found->second == target.id) throw ScenarioLoadError("turnaround task cannot depend on itself");
                    target.prerequisites.push_back(found->second);
                }
                if (!target.prerequisites.empty()) target.status = TaskStatus::Blocked;
            }
            std::vector<int> marks(source.tasks.size(), 0);
            auto visit = [&](auto&& self, std::size_t current) -> void {
                if (marks[current] == 1) throw ScenarioLoadError(std::format("aircraft '{}' service task graph contains a cycle", source.id));
                if (marks[current] == 2) return;
                marks[current] = 1;
                for (const auto& prerequisite : source.tasks[current].prerequisites) {
                    const auto found = std::ranges::find_if(source.tasks, [&](const auto& task) { return task.id == prerequisite; });
                    if (found == source.tasks.end()) continue;
                    self(self, static_cast<std::size_t>(found - source.tasks.begin()));
                }
                marks[current] = 2;
            };
            for (std::size_t task_index = 0; task_index < source.tasks.size(); ++task_index) visit(visit, task_index);
        }
    }
    for (const auto& value : document.road_events) {
        if (value.time < 0) throw ScenarioLoadError("road event timestamp cannot be negative");
        scenario.road_events.push_back({SimTime{value.time},
            lookup(edges, value.edge, "road event edge"), value.enabled});
    }
    for (const auto& value : document.disruptions) {
        if (value.time < 0 || value.duration <= 0) throw ScenarioLoadError("turnaround disruption has invalid timing");
        const auto aircraft_found = std::ranges::find_if(document.aircraft,
            [&](const auto& flight) { return flight.id == value.aircraft; });
        if (aircraft_found == document.aircraft.end()) throw ScenarioLoadError(std::format("disruption references missing aircraft '{}'", value.aircraft));
        const auto task_found = aircraft_task_ids[value.aircraft].find(value.task);
        if (task_found == aircraft_task_ids[value.aircraft].end()) throw ScenarioLoadError(std::format("disruption references missing task '{}.{}'", value.aircraft, value.task));
        scenario.task_duration_disruptions.push_back({SimTime{value.time}, task_found->second, SimTime{value.duration}});
    }
    for (const auto& value : document.vehicle_outages) {
        if (value.time < 0) throw ScenarioLoadError("vehicle outage timestamp cannot be negative");
        const auto vehicle_found = std::ranges::find(document.vehicles, value.vehicle, &VehicleDocument::id);
        if (vehicle_found == document.vehicles.end())
            throw ScenarioLoadError(std::format("vehicle outage references missing vehicle '{}'", value.vehicle));
        const auto index = static_cast<std::size_t>(vehicle_found - document.vehicles.begin());
        scenario.vehicle_outages.push_back({SimTime{value.time}, VehicleId{static_cast<std::uint32_t>(index + 1)}});
    }

    for (const auto& flight : scenario.aircraft) {
        for (const auto& task : flight.tasks()) {
            if (task.type != ServiceType::Fueling && task.type != ServiceType::Baggage) continue;
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
