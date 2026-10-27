#include "airside/scenario/scenario_loader.hpp"

#include "airside/routing/astar.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cstdint>
#include <cmath>
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
    std::vector<Point2d> centerline; bool aircraft_allowed{true}; bool ground_vehicle_allowed{true}; bool crosses_runway{false};
    std::string surface_type{"unclassified"}; double width_m{0.0}; double minimum_clearance_m{0.0};
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
    bool arrival_only{false}; bool arrival_turnaround{false}; std::string arrival_exit;
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
    struct DesignRunwayExit { std::string id; std::string node; std::string connector_edge; };
    struct DesignRunway {
        std::string id; std::string designation; Point2d threshold_09; Point2d threshold_27;
        double length_m; double width_m; std::string surface_type; std::string departure_hold_node;
        std::vector<DesignRunwayExit> arrival_exits;
    };
    struct DesignHoldPoint { std::string id; std::string node; double protected_clearance_m; };
    struct DesignStand {
        std::string id; std::string gate; std::string node; std::vector<Point2d> footprint;
        double length_m; double width_m; double maximum_wingspan_m; double wingtip_clearance_m;
        std::string pushback_direction; bool tug_required; std::string aircraft_access_edge; std::string service_access_edge;
    };
    struct DesignArea { std::string id; std::string kind; std::string node; std::vector<Point2d> footprint; };
    struct DesignRegion {
        std::string id; std::string kind; std::string surface_type; double elevation_m;
        std::vector<Point2d> footprint;
    };
    struct AirportDesign {
        std::string name; bool fictional; std::string units; std::string x_axis; std::string y_axis; std::string z_axis;
        std::string origin_node; double unreal_centimeters_per_meter;
        std::vector<DesignRunway> runways; std::vector<DesignHoldPoint> hold_points;
        std::vector<DesignStand> stands; std::vector<DesignArea> support_areas; std::vector<DesignRegion> regions;
    };
    std::optional<AirportDesign> airport_design;
    struct SurfaceOperations { std::string departure_handoff; std::string runway_node; std::string arrival_exit; std::int64_t pushback_seconds{30}; std::int64_t runway_seconds{60}; std::int64_t arrival_rollout_seconds{90}; double speed_mps{5.0}; double queue_spacing_m{30.0}; bool return_vehicles_after_task{false}; std::string aircraft_routing_policy{"SCRIPTED"}; };
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

Point2d parse_point(const YAML::Node& point, std::string_view context) {
    return {scalar<double>(point, "x_m", context), scalar<double>(point, "y_m", context)};
}

std::vector<Point2d> parse_footprint(const YAML::Node& points, std::string_view context) {
    if (!points.IsSequence() || points.size() < 3)
        throw ScenarioLoadError(std::format("{} must contain at least three points", context));
    std::vector<Point2d> result;
    result.reserve(points.size());
    for (const auto& point : points) result.push_back(parse_point(point, context));
    return result;
}

void require_sequence(const YAML::Node& node, std::string_view context) {
    if (!node.IsSequence() || node.size() == 0) {
        throw ScenarioLoadError(std::format("{} must be a non-empty sequence", context));
    }
}

double point_distance(Point2d a, Point2d b) {
    return std::hypot(a.x_m - b.x_m, a.y_m - b.y_m);
}

double distance_to_segment(Point2d point, Point2d start, Point2d end) {
    const double dx = end.x_m - start.x_m;
    const double dy = end.y_m - start.y_m;
    const double squared_length = dx * dx + dy * dy;
    if (squared_length <= 0.0) return point_distance(point, start);
    const double fraction = std::clamp(((point.x_m - start.x_m) * dx + (point.y_m - start.y_m) * dy) /
        squared_length, 0.0, 1.0);
    return point_distance(point, {start.x_m + fraction * dx, start.y_m + fraction * dy});
}

double polygon_area(const std::vector<Point2d>& footprint) {
    double twice_area = 0.0;
    for (std::size_t index = 0; index < footprint.size(); ++index) {
        const auto& current = footprint[index];
        const auto& next = footprint[(index + 1) % footprint.size()];
        twice_area += current.x_m * next.y_m - next.x_m * current.y_m;
    }
    return std::abs(twice_area) * 0.5;
}

bool contains_point(const std::vector<Point2d>& footprint, Point2d point) {
    bool inside = false;
    for (std::size_t i = 0, j = footprint.size() - 1; i < footprint.size(); j = i++) {
        const auto& a = footprint[i];
        const auto& b = footprint[j];
        const bool crosses = ((a.y_m > point.y_m) != (b.y_m > point.y_m)) &&
            (point.x_m < (b.x_m - a.x_m) * (point.y_m - a.y_m) / (b.y_m - a.y_m) + a.x_m);
        if (crosses) inside = !inside;
    }
    return inside;
}

void validate_footprint(const std::vector<Point2d>& footprint, std::string_view context) {
    if (footprint.size() < 3 || polygon_area(footprint) <= 1e-6 ||
        std::ranges::any_of(footprint, [](Point2d p) { return !std::isfinite(p.x_m) || !std::isfinite(p.y_m); }))
        throw ScenarioLoadError(std::format("{} must be a finite, non-degenerate polygon", context));
}

ScenarioDocument parse_document(const YAML::Node& root) {
    if (!root.IsMap()) throw ScenarioLoadError("scenario root must be a mapping");
    ScenarioDocument document;
    document.name = scalar<std::string>(root, "name", "scenario");
    document.default_seed = root["default_seed"] ?
        scalar<std::uint64_t>(root, "default_seed", "scenario") : 42U;

    const auto airport = required(root, "airport", "scenario");
    if (const auto design = airport["design"]) {
        ScenarioDocument::AirportDesign value;
        value.name = scalar<std::string>(design, "airport_name", "airport.design");
        value.fictional = scalar<bool>(design, "fictional", "airport.design");
        const auto frame = required(design, "coordinate_frame", "airport.design");
        value.units = scalar<std::string>(frame, "units", "airport.design.coordinate_frame");
        value.x_axis = scalar<std::string>(frame, "x_axis", "airport.design.coordinate_frame");
        value.y_axis = scalar<std::string>(frame, "y_axis", "airport.design.coordinate_frame");
        value.z_axis = scalar<std::string>(frame, "z_axis", "airport.design.coordinate_frame");
        value.origin_node = scalar<std::string>(frame, "origin_node", "airport.design.coordinate_frame");
        value.unreal_centimeters_per_meter = scalar<double>(design, "unreal_centimeters_per_meter", "airport.design");

        const auto runways = required(design, "runways", "airport.design");
        require_sequence(runways, "airport.design.runways");
        for (std::size_t index = 0; index < runways.size(); ++index) {
            const auto runway = runways[index];
            const auto context = std::format("airport.design.runways[{}]", index);
            ScenarioDocument::DesignRunway parsed{
                scalar<std::string>(runway, "id", context),
                scalar<std::string>(runway, "designation", context),
                parse_point(required(runway, "threshold_09", context), context),
                parse_point(required(runway, "threshold_27", context), context),
                scalar<double>(runway, "length_m", context),
                scalar<double>(runway, "width_m", context),
                scalar<std::string>(runway, "surface_type", context),
                scalar<std::string>(runway, "departure_hold_node", context), {}};
            const auto exits = required(runway, "arrival_exits", context);
            require_sequence(exits, std::format("{}.arrival_exits", context));
            for (const auto& exit : exits) parsed.arrival_exits.push_back({
                scalar<std::string>(exit, "id", context),
                scalar<std::string>(exit, "node", context),
                scalar<std::string>(exit, "connector_edge", context)});
            value.runways.push_back(std::move(parsed));
        }

        if (const auto holds = design["hold_points"]) {
            require_sequence(holds, "airport.design.hold_points");
            for (const auto& hold : holds) value.hold_points.push_back({
                scalar<std::string>(hold, "id", "airport.design.hold_points"),
                scalar<std::string>(hold, "node", "airport.design.hold_points"),
                scalar<double>(hold, "protected_clearance_m", "airport.design.hold_points")});
        }
        if (const auto stands = design["stands"]) {
            require_sequence(stands, "airport.design.stands");
            for (const auto& stand : stands) {
                const auto context = "airport.design.stands";
                value.stands.push_back({
                    scalar<std::string>(stand, "id", context),
                    scalar<std::string>(stand, "gate", context),
                    scalar<std::string>(stand, "node", context),
                    parse_footprint(required(stand, "footprint", context), context),
                    scalar<double>(stand, "length_m", context),
                    scalar<double>(stand, "width_m", context),
                    scalar<double>(stand, "maximum_wingspan_m", context),
                    scalar<double>(stand, "wingtip_clearance_m", context),
                    scalar<std::string>(stand, "pushback_direction", context),
                    scalar<bool>(stand, "tug_required", context),
                    scalar<std::string>(stand, "aircraft_access_edge", context),
                    scalar<std::string>(stand, "service_access_edge", context)});
            }
        }
        if (const auto areas = design["support_areas"]) {
            require_sequence(areas, "airport.design.support_areas");
            for (const auto& area : areas) value.support_areas.push_back({
                scalar<std::string>(area, "id", "airport.design.support_areas"),
                scalar<std::string>(area, "kind", "airport.design.support_areas"),
                scalar<std::string>(area, "node", "airport.design.support_areas"),
                parse_footprint(required(area, "footprint", "airport.design.support_areas"), "airport.design.support_areas")});
        }
        if (const auto regions = design["regions"]) {
            require_sequence(regions, "airport.design.regions");
            for (const auto& region : regions) value.regions.push_back({
                scalar<std::string>(region, "id", "airport.design.regions"),
                scalar<std::string>(region, "kind", "airport.design.regions"),
                scalar<std::string>(region, "surface_type", "airport.design.regions"),
                scalar<double>(region, "elevation_m", "airport.design.regions"),
                parse_footprint(required(region, "footprint", "airport.design.regions"), "airport.design.regions")});
        }
        document.airport_design = std::move(value);
    }
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
        EdgeDocument edge{scalar<std::string>(item, "id", context),
            scalar<std::string>(item, "from", context), scalar<std::string>(item, "to", context),
            scalar<double>(item, "distance_m", context),
            scalar<std::int64_t>(item, "traversal_time_seconds", context),
            optional_bool(item, "enabled", true, context), optional_bool(item, "one_way", false, context)};
        edge.aircraft_allowed = optional_bool(item, "aircraft_allowed", true, context);
        edge.ground_vehicle_allowed = optional_bool(item, "ground_vehicle_allowed", true, context);
        edge.crosses_runway = optional_bool(item, "surface_route_crosses_runway", false, context);
        if (item["surface_type"]) edge.surface_type = scalar<std::string>(item, "surface_type", context);
        if (item["width_m"]) edge.width_m = scalar<double>(item, "width_m", context);
        if (item["minimum_clearance_m"])
            edge.minimum_clearance_m = scalar<double>(item, "minimum_clearance_m", context);
        if (item["surface_centerline"]) {
            if (!item["surface_centerline"].IsSequence() || item["surface_centerline"].size() < 2)
                throw ScenarioLoadError(context + ".surface_centerline must contain at least two points");
            for (const auto& point : item["surface_centerline"])
                edge.centerline.push_back({scalar<double>(point, "x_m", context), scalar<double>(point, "y_m", context)});
        }
        document.edges.push_back(std::move(edge));
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
    if (!vehicles.IsSequence()) throw ScenarioLoadError("fleet.vehicles must be a sequence");
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
        if (operation_type != "turnaround" && operation_type != "arrival" && operation_type != "arrival_turnaround")
            throw ScenarioLoadError(std::format("{}: operation_type must be 'turnaround', 'arrival', or 'arrival_turnaround'", context));
        const auto services = item["required_services"];
        const auto tasks = item["service_tasks"];
        if (operation_type != "arrival" && !services && !tasks) throw ScenarioLoadError(std::format("{}: missing 'required_services' or 'service_tasks'", context));
        if (services) require_sequence(services, std::format("{}.required_services", context));
        if (tasks && !tasks.IsSequence()) throw ScenarioLoadError(std::format("{}.service_tasks must be a sequence", context));
        if (tasks && tasks.size() == 0) throw ScenarioLoadError(std::format("{}.service_tasks cannot be empty", context));
        AircraftDocument flight;
        flight.id = scalar<std::string>(item, "id", context);
        flight.arrival_only = operation_type == "arrival";
        flight.arrival_turnaround = operation_type == "arrival_turnaround";
        if (flight.arrival_only || flight.arrival_turnaround) flight.arrival_exit = scalar<std::string>(item, "arrival_exit", context);
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
    for (const auto& name : {"fueling", "baggage", "deboarding", "catering", "cabin_cleaning", "baggage_load", "pushback_preparation", "lavatory_service", "potable_water", "ground_power", "pushback", "passenger_stairs", "air_start", "maintenance_inspection", "chocks"}) {
        if (durations[name]) document.service_durations.emplace(name, scalar<std::int64_t>(durations, name, "service_durations_seconds"));
    }
    if (document.service_durations.empty()) {
        throw ScenarioLoadError("service_durations_seconds requires at least one service duration");
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
        config.return_vehicles_after_task = optional_bool(surface, "return_service_vehicles_to_depot_after_task", false, "surface_operations");
        if (surface["aircraft_routing_policy"])
            config.aircraft_routing_policy = scalar<std::string>(surface, "aircraft_routing_policy", "surface_operations");
        if (config.aircraft_routing_policy != "SCRIPTED" && config.aircraft_routing_policy != "ADAPTIVE" &&
            config.aircraft_routing_policy != "COORDINATED_ADAPTIVE" &&
            config.aircraft_routing_policy != "ROLLING_HORIZON_COORDINATED" &&
            config.aircraft_routing_policy != "COUNTERFACTUAL" &&
            config.aircraft_routing_policy != "COST_AWARE_COUNTERFACTUAL")
            throw ScenarioLoadError("surface_operations.aircraft_routing_policy must be SCRIPTED, ADAPTIVE, COORDINATED_ADAPTIVE, ROLLING_HORIZON_COORDINATED, COUNTERFACTUAL, or COST_AWARE_COUNTERFACTUAL");
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
    if (value == "lavatory_service") return ServiceType::LavatoryService;
    if (value == "potable_water") return ServiceType::PotableWater;
    if (value == "ground_power") return ServiceType::GroundPower;
    if (value == "pushback") return ServiceType::Pushback;
    if (value == "passenger_stairs") return ServiceType::PassengerStairs;
    if (value == "air_start") return ServiceType::AirStart;
    if (value == "maintenance_inspection") return ServiceType::MaintenanceInspection;
    if (value == "chocks") return ServiceType::Chocks;
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
    case ServiceType::LavatoryService: return "lavatory_service";
    case ServiceType::PotableWater: return "potable_water";
    case ServiceType::GroundPower: return "ground_power";
    case ServiceType::Pushback: return "pushback";
    case ServiceType::PassengerStairs: return "passenger_stairs";
    case ServiceType::AirStart: return "air_start";
    case ServiceType::MaintenanceInspection: return "maintenance_inspection";
    case ServiceType::Chocks: return "chocks";
    }
    return "";
}

std::string service_resource(ServiceType type) {
    switch (type) {
    case ServiceType::Fueling: return "fuel_truck";
    case ServiceType::Baggage: case ServiceType::BaggageLoad: return "baggage_vehicle";
    case ServiceType::Catering: return "catering_truck";
    case ServiceType::LavatoryService: return "lavatory_truck";
    case ServiceType::PotableWater: return "water_truck";
    case ServiceType::GroundPower: return "gpu";
    case ServiceType::Pushback: return "pushback_tug";
    case ServiceType::PassengerStairs: return "passenger_stairs";
    case ServiceType::AirStart: return "air_start_unit";
    default: return "abstract_crew";
    }
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
            SimTime{config.arrival_rollout_seconds}, config.speed_mps, config.queue_spacing_m,
            config.return_vehicles_after_task, agents::parse_autonomy_mode(config.aircraft_routing_policy)};
    }
    for (std::size_t index = 0; index < document.edges.size(); ++index) {
        const auto& value = document.edges[index];
        if (value.from == value.to || value.distance <= 0.0 || value.traversal_seconds <= 0) {
            throw ScenarioLoadError(std::format("edge '{}' has invalid geometry or traversal time", value.id));
        }
        const auto id = EdgeId{static_cast<std::uint32_t>(index + 1)};
        edges.emplace(value.id, id);
        RoadEdge edge{id, lookup(nodes, value.from, std::format("edge '{}'.from", value.id)),
            lookup(nodes, value.to, std::format("edge '{}'.to", value.id)), value.distance,
            SimTime{value.traversal_seconds}, value.enabled, value.one_way};
        edge.centerline = value.centerline;
        edge.aircraft_allowed = value.aircraft_allowed;
        edge.ground_vehicle_allowed = value.ground_vehicle_allowed;
        edge.crosses_runway = value.crosses_runway;
        edge.surface_type = value.surface_type;
        edge.width_m = value.width_m;
        edge.minimum_clearance_m = value.minimum_clearance_m;
        if (!std::isfinite(edge.width_m) || !std::isfinite(edge.minimum_clearance_m) ||
            edge.width_m < 0.0 || edge.minimum_clearance_m < 0.0 ||
            (edge.surface_type != "unclassified" && edge.width_m <= 0.0))
            throw ScenarioLoadError(std::format("edge '{}' has invalid surface dimensions", value.id));
        scenario.graph.add_edge(std::move(edge));
    }
    for (std::size_t index = 0; index < document.gates.size(); ++index) {
        const auto& value = document.gates[index];
        const auto id = GateId{static_cast<std::uint32_t>(index + 1)};
        gates.emplace(value.id, id);
        scenario.gates.emplace_back(id, value.name,
            lookup(nodes, value.node, std::format("gate '{}'.node", value.id)), value.enabled);
    }

    if (document.airport_design) {
        const auto& source = *document.airport_design;
        if (source.name.empty() || !source.fictional || source.units != "meters" ||
            source.x_axis != "east" || source.y_axis != "north" || source.z_axis != "up" ||
            !std::isfinite(source.unreal_centimeters_per_meter) || source.unreal_centimeters_per_meter <= 0.0)
            throw ScenarioLoadError("airport.design must be fictional, metric, right-handed east/north/up, and have a positive Unreal scale");
        const auto origin = lookup(nodes, source.origin_node, "airport.design.coordinate_frame.origin_node");
        if (point_distance(scenario.graph.node(origin).position, {}) > 1e-6)
            throw ScenarioLoadError("airport.design origin node must be at local (0, 0)");

        AirportDesign design;
        design.name = source.name;
        design.fictional = source.fictional;
        design.coordinate_units = source.units;
        design.x_axis = source.x_axis;
        design.y_axis = source.y_axis;
        design.z_axis = source.z_axis;
        design.origin_node = origin;
        design.unreal_centimeters_per_meter = source.unreal_centimeters_per_meter;
        std::unordered_set<std::string> runway_ids;
        for (const auto& value : source.runways) {
            if (value.id.empty() || !runway_ids.insert(value.id).second || value.designation.empty() ||
                value.surface_type.empty() || !std::isfinite(value.length_m) || !std::isfinite(value.width_m) ||
                !std::isfinite(value.threshold_09.x_m) || !std::isfinite(value.threshold_09.y_m) ||
                !std::isfinite(value.threshold_27.x_m) || !std::isfinite(value.threshold_27.y_m) ||
                value.length_m <= 0.0 || value.width_m <= 0.0)
                throw ScenarioLoadError("airport.design contains an invalid or duplicate runway definition");
            const double measured_length = point_distance(value.threshold_09, value.threshold_27);
            if (std::abs(measured_length - value.length_m) > 0.05)
                throw ScenarioLoadError(std::format("runway '{}' threshold distance does not match length_m", value.id));
            const auto hold_node = lookup(nodes, value.departure_hold_node,
                std::format("runway '{}'.departure_hold_node", value.id));
            const auto hold_position = scenario.graph.node(hold_node).position;
            if (distance_to_segment(hold_position, value.threshold_09, value.threshold_27) <= value.width_m * 0.5)
                throw ScenarioLoadError(std::format("runway '{}' departure hold node must be outside runway pavement", value.id));
            AirportRunway runway{value.id, value.designation, value.threshold_09, value.threshold_27,
                value.length_m, value.width_m, value.surface_type, hold_node, {}};
            std::unordered_set<std::string> exit_ids;
            for (const auto& exit : value.arrival_exits) {
                const auto node = lookup(nodes, exit.node, std::format("runway '{}'.arrival_exit", value.id));
                const auto edge_id = lookup(edges, exit.connector_edge,
                    std::format("runway '{}'.arrival_exit.connector_edge", value.id));
                const auto& connector = scenario.graph.edge(edge_id);
                const auto position = scenario.graph.node(node).position;
                if (exit.id.empty() || !exit_ids.insert(exit.id).second ||
                    distance_to_segment(position, value.threshold_09, value.threshold_27) > value.width_m * 0.5 ||
                    (connector.from != node && connector.to != node) || !connector.aircraft_allowed ||
                    connector.ground_vehicle_allowed || connector.crosses_runway)
                    throw ScenarioLoadError(std::format("runway '{}' has an invalid aircraft-only arrival exit", value.id));
                runway.arrival_exits.push_back({exit.id, node, edge_id});
            }
            if (runway.arrival_exits.size() < 2)
                throw ScenarioLoadError(std::format("runway '{}' requires at least two arrival exits", value.id));
            design.runways.push_back(std::move(runway));
        }
        if (design.runways.empty()) throw ScenarioLoadError("airport.design requires at least one runway");

        std::unordered_set<std::string> hold_ids;
        for (const auto& value : source.hold_points) {
            const auto node = lookup(nodes, value.node, "airport.design.hold_points.node");
            if (value.id.empty() || !hold_ids.insert(value.id).second ||
                !std::isfinite(value.protected_clearance_m) || value.protected_clearance_m <= 0.0)
                throw ScenarioLoadError("airport.design contains an invalid or duplicate hold point");
            design.hold_points.push_back({value.id, node, value.protected_clearance_m});
        }

        std::unordered_set<std::string> stand_ids;
        std::unordered_set<GateId> described_gates;
        for (const auto& value : source.stands) {
            const auto gate_id = lookup(gates, value.gate, std::format("stand '{}'.gate", value.id));
            const auto node = lookup(nodes, value.node, std::format("stand '{}'.node", value.id));
            const auto aircraft_edge_id = lookup(edges, value.aircraft_access_edge,
                std::format("stand '{}'.aircraft_access_edge", value.id));
            const auto service_edge_id = lookup(edges, value.service_access_edge,
                std::format("stand '{}'.service_access_edge", value.id));
            const auto& aircraft_edge = scenario.graph.edge(aircraft_edge_id);
            const auto& service_edge = scenario.graph.edge(service_edge_id);
            const auto gate = std::ranges::find(scenario.gates, gate_id, &Gate::id);
            validate_footprint(value.footprint, std::format("stand '{}' footprint", value.id));
            if (value.id.empty() || !stand_ids.insert(value.id).second || !described_gates.insert(gate_id).second ||
                gate == scenario.gates.end() || gate->node != node || !contains_point(value.footprint, scenario.graph.node(node).position) ||
                !std::isfinite(value.length_m) || !std::isfinite(value.width_m) ||
                !std::isfinite(value.maximum_wingspan_m) || !std::isfinite(value.wingtip_clearance_m) ||
                value.length_m <= 0.0 || value.width_m <= 0.0 || value.maximum_wingspan_m <= 0.0 ||
                value.wingtip_clearance_m < 0.0 || value.pushback_direction.empty() ||
                (aircraft_edge.from != node && aircraft_edge.to != node) || !aircraft_edge.aircraft_allowed ||
                (service_edge.from != node && service_edge.to != node) || !service_edge.ground_vehicle_allowed ||
                service_edge.aircraft_allowed)
                throw ScenarioLoadError(std::format("stand '{}' has invalid geometry, compatibility, or access edges", value.id));
            design.stands.push_back({value.id, gate_id, node, value.footprint, value.length_m, value.width_m,
                value.maximum_wingspan_m, value.wingtip_clearance_m, value.pushback_direction, value.tug_required,
                aircraft_edge_id, service_edge_id});
        }
        if (design.stands.size() != scenario.gates.size())
            throw ScenarioLoadError("airport.design must define exactly one stand for every gate");

        std::unordered_set<std::string> support_ids;
        for (const auto& value : source.support_areas) {
            const auto node = lookup(nodes, value.node, std::format("support area '{}'.node", value.id));
            validate_footprint(value.footprint, std::format("support area '{}' footprint", value.id));
            if (value.id.empty() || !support_ids.insert(value.id).second || value.kind.empty() ||
                !contains_point(value.footprint, scenario.graph.node(node).position))
                throw ScenarioLoadError("airport.design contains an invalid support area");
            design.support_areas.push_back({value.id, value.kind, node, value.footprint});
        }
        std::unordered_set<std::string> region_ids;
        for (const auto& value : source.regions) {
            validate_footprint(value.footprint, std::format("surface region '{}' footprint", value.id));
            if (value.id.empty() || !region_ids.insert(value.id).second || value.kind.empty() ||
                value.surface_type.empty() || !std::isfinite(value.elevation_m))
                throw ScenarioLoadError("airport.design contains an invalid surface region");
            design.regions.push_back({value.id, value.kind, value.surface_type, value.elevation_m, value.footprint});
        }
        for (const auto& edge : scenario.graph.edges()) {
            if (edge.surface_type != "unclassified" && edge.width_m <= 0.0)
                throw ScenarioLoadError("airport design route edges require positive surface widths");
            if (edge.centerline.size() >= 2) {
                const auto& from = scenario.graph.node(edge.from).position;
                const auto& to = scenario.graph.node(edge.to).position;
                if (point_distance(edge.centerline.front(), from) > 0.05 ||
                    point_distance(edge.centerline.back(), to) > 0.05 ||
                    std::abs(edge.centerline_length_m - edge.distance_m) > 0.05)
                    throw ScenarioLoadError(std::format("edge {} centerline does not match graph endpoints/distance", edge.id.value()));
            }
        }
        if (!scenario.surface_operations) throw ScenarioLoadError("airport.design requires surface_operations");
        for (const auto& runway : design.runways) {
            const auto hold = std::ranges::find(design.hold_points, runway.departure_hold_node, &AirportHoldPoint::node);
            if (hold == design.hold_points.end() || scenario.surface_operations->runway_node != runway.departure_hold_node)
                throw ScenarioLoadError(std::format("runway '{}' departure hold metadata disagrees with surface_operations", runway.id));
            for (const auto& gate : scenario.gates) {
                if (!find_route(scenario.graph, gate.node, runway.departure_hold_node, MovementClass::Aircraft) ||
                    !find_route(scenario.graph, runway.departure_hold_node, gate.node, MovementClass::Aircraft))
                    throw ScenarioLoadError(std::format("runway '{}' and gate '{}' are not connected for aircraft", runway.id, gate.name));
            }
            for (const auto& exit : runway.arrival_exits) {
                for (const auto& gate : scenario.gates)
                    if (!find_route(scenario.graph, exit.runway_node, gate.node, MovementClass::Aircraft))
                        throw ScenarioLoadError(std::format("runway exit '{}' cannot reach gate '{}'", exit.id, gate.name));
            }
            for (const auto& vehicle : document.vehicles) {
                const auto depot = lookup(nodes, vehicle.depot, std::format("vehicle '{}'.depot_node", vehicle.id));
                for (const auto& gate : scenario.gates) {
                    if (!find_route(scenario.graph, depot, gate.node, MovementClass::GroundVehicle) ||
                        !find_route(scenario.graph, gate.node, depot, MovementClass::GroundVehicle))
                        throw ScenarioLoadError(std::format("GSE vehicle '{}' cannot travel both ways between depot and gate '{}'", vehicle.id, gate.name));
                }
                if (find_route(scenario.graph, depot, runway.departure_hold_node, MovementClass::GroundVehicle))
                    throw ScenarioLoadError(std::format("GSE vehicle '{}' has a route to runway hold point", vehicle.id));
            }
        }
        scenario.airport_design = std::move(design);
    }

    for (std::size_t index = 0; index < document.vehicles.size(); ++index) {
        const auto& value = document.vehicles[index];
        if (value.speed <= 0.0) {
            throw ScenarioLoadError(std::format("vehicle '{}' speed_mps must be positive", value.id));
        }
        const auto type = parse_service(value.type, std::format("vehicle '{}'", value.id));
        scenario.vehicles.emplace_back(VehicleId{static_cast<std::uint32_t>(index + 1)}, value.name,
            type, lookup(nodes, value.depot, std::format("vehicle '{}'.depot_node", value.id)), value.speed,
            value.outage_safe_node ? std::optional<NodeId>{lookup(nodes, *value.outage_safe_node,
                std::format("vehicle '{}'.outage_safe_node", value.id))} : std::nullopt);
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
            const auto expected_resource = service_resource(type);
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
        if ((value.arrival_only || value.arrival_turnaround) && !scenario.surface_operations)
            throw ScenarioLoadError(std::format("arrival aircraft '{}' requires surface_operations", value.id));
        scenario.aircraft.emplace_back(AircraftId{static_cast<std::uint32_t>(index + 1)}, value.id,
            SimTime{value.arrival}, SimTime{value.departure}, gate_id, gate_it->node, std::move(tasks),
            value.turnaround_id, SimTime{value.target_off_block},
            value.arrival_only ? AircraftOperationType::ArrivalOnly :
                value.arrival_turnaround ? AircraftOperationType::ArrivalTurnaround : AircraftOperationType::Turnaround,
            (value.arrival_only || value.arrival_turnaround)
                ? lookup(nodes, value.arrival_exit, std::format("aircraft '{}'.arrival_exit", value.id)) : NodeId{});
    }
    if (scenario.airport_design) {
        for (const auto& runway : scenario.airport_design->runways)
            for (const auto& flight : scenario.aircraft)
                if (flight.operation_type() != AircraftOperationType::Turnaround &&
                    std::ranges::none_of(runway.arrival_exits, [&](const auto& exit) { return exit.runway_node == flight.arrival_exit_node(); }))
                    throw ScenarioLoadError(std::format("aircraft '{}' uses an arrival exit absent from runway design", flight.flight_number()));
    }
    for (const auto& [name, seconds] : document.service_durations) {
        const auto type = parse_service(name, "service_durations_seconds");
        scenario.service_durations.emplace(type, SimTime{seconds});
    }
    for (const auto& type : {ServiceType::Fueling, ServiceType::Baggage, ServiceType::Deboarding,
                             ServiceType::Catering, ServiceType::CabinCleaning, ServiceType::BaggageLoad,
                             ServiceType::PushbackPreparation, ServiceType::LavatoryService, ServiceType::PotableWater,
                             ServiceType::GroundPower, ServiceType::Pushback, ServiceType::PassengerStairs,
                             ServiceType::AirStart, ServiceType::MaintenanceInspection, ServiceType::Chocks})
        if (!scenario.service_durations.contains(type)) scenario.service_durations.emplace(type, SimTime{300});
    for (const auto& type : {ServiceType::Deboarding, ServiceType::CabinCleaning,
                             ServiceType::PushbackPreparation, ServiceType::MaintenanceInspection, ServiceType::Chocks})
        scenario.abstract_resource_capacity.emplace(type, 1U);
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
                    find_route(scenario.graph, vehicle.depot_node(), flight.gate_node(), MovementClass::GroundVehicle).has_value();
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
