#include "airside/autonomy/fleet.hpp"
#include "airside/autonomy/dispatcher.hpp"
#include "airside/routing/astar.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <ranges>
#include <set>
#include <tuple>

namespace airside::autonomy {
namespace {
double separation(Vec2 a, Vec2 b) { return std::hypot(a.x_m-b.x_m, a.y_m-b.y_m); }
Vec2 velocity(const EstimatedState& s) { return {s.speed_mps*std::cos(s.heading_rad),s.speed_mps*std::sin(s.heading_rad)}; }
AutonomyScenario mission_scenario(AutonomyScenario s,const FleetMission& m) {
    s.name += "_"+m.id.value; s.start_node=m.start_node; s.goal_node=m.goal_node;
    const AirportNode* start=nullptr; const AirportNode* goal=nullptr;
    for(const auto& n:s.airport.graph.nodes()){if(n.name==m.start_node)start=&n;if(n.name==m.goal_node)goal=&n;}
    if(!start||!goal)throw std::invalid_argument("fleet mission references an unknown road node");
    s.initial_state.position=start->position;
    s.initial_state.heading_rad=std::atan2(goal->position.y_m-start->position.y_m,goal->position.x_m-start->position.x_m);
    s.faults.insert(s.faults.end(),m.faults.begin(),m.faults.end());
    return s;
}

std::vector<FleetMission> idle_missions(const std::vector<DispatchVehicle>& vehicles) {
    std::vector<DispatchVehicle> sorted = vehicles;
    std::ranges::sort(sorted, {}, [](const auto& vehicle) { return vehicle.id; });
    std::vector<FleetMission> missions;
    missions.reserve(sorted.size());
    for (const auto& vehicle : sorted)
        missions.push_back({vehicle.id, vehicle.current_node, vehicle.current_node, 0, {}});
    return missions;
}

std::vector<std::string> route_resources(const AutonomyScenario& scenario,const FleetMission& mission) {
    std::optional<NodeId> start,goal;
    for(const auto& node:scenario.airport.graph.nodes()){
        if(node.name==mission.start_node)start=node.id;
        if(node.name==mission.goal_node)goal=node.id;
    }
    if(!start||!goal)throw std::invalid_argument("fleet mission references an unknown road node");
    const auto route=airside::find_route(scenario.airport.graph,*start,*goal);
    if(!route)throw std::invalid_argument("fleet mission has no route through the airport road graph");
    std::vector<std::string> keys;
    keys.reserve(route->edges.size()+route->nodes.size());
    for(const auto edge:route->edges)keys.push_back("edge/"+std::to_string(edge.value()));
    for(const auto node:route->nodes)keys.push_back("intersection/"+std::to_string(node.value()));
    return keys;
}

struct DirectedRouteEdge { std::string resource; NodeId from; NodeId to; };

std::vector<DirectedRouteEdge> directed_route_edges(
    const AutonomyScenario& scenario, const FleetMission& mission) {
    std::optional<NodeId> start, goal;
    for (const auto& node : scenario.airport.graph.nodes()) {
        if (node.name == mission.start_node) start = node.id;
        if (node.name == mission.goal_node) goal = node.id;
    }
    if (!start || !goal) throw std::invalid_argument("fleet mission references an unknown road node");
    const auto route = airside::find_route(scenario.airport.graph, *start, *goal);
    if (!route) throw std::invalid_argument("fleet mission has no route through the airport road graph");
    std::vector<DirectedRouteEdge> traversals;
    traversals.reserve(route->edges.size());
    for (std::size_t i = 0; i < route->edges.size(); ++i)
        traversals.push_back({"edge/" + std::to_string(route->edges[i].value()), route->nodes[i], route->nodes[i + 1]});
    return traversals;
}

std::vector<std::string> opposing_shared_edges(
    const AutonomyScenario& scenario, const FleetMission& a, const FleetMission& b) {
    const auto a_route = directed_route_edges(scenario, a);
    const auto b_route = directed_route_edges(scenario, b);
    std::vector<std::string> shared;
    for (const auto& a_edge : a_route) {
        for (const auto& b_edge : b_route) {
            if (a_edge.resource == b_edge.resource && a_edge.from == b_edge.to && a_edge.to == b_edge.from)
                shared.push_back(a_edge.resource);
        }
    }
    std::ranges::sort(shared);
    shared.erase(std::unique(shared.begin(), shared.end()), shared.end());
    return shared;
}

std::string shared_route_resource(const std::vector<std::string>& a,const std::vector<std::string>& b,
                                 const VehicleId& a_id,const VehicleId& b_id) {
    for(const auto& key:a)if(key.starts_with("edge/")&&std::ranges::find(b,key)!=b.end())return key;
    for(const auto& key:a)if(key.starts_with("intersection/")&&std::ranges::find(b,key)!=b.end())return key;
    return "vehicle_conflict/"+a_id.value+"/"+b_id.value;
}

}

namespace {
std::uint64_t resource_vehicle_order(const std::string& resource, const VehicleId& vehicle) {
    std::uint64_t vehicle_hash = 14695981039346656037ULL;
    for (const unsigned char byte : vehicle.value) {
        vehicle_hash ^= byte;
        vehicle_hash *= 1099511628211ULL;
    }
    std::uint64_t resource_key = 0;
    const auto separator = resource.find_last_of('/');
    if (separator != std::string::npos) {
        try { resource_key = std::stoull(resource.substr(separator + 1)); }
        catch (...) { }
    }
    if (resource_key == 0) {
        for (const unsigned char byte : resource) {
            resource_key ^= byte;
            resource_key *= 1099511628211ULL;
        }
    } else {
        --resource_key;
    }
    return vehicle_hash ^ (resource_key * 0x9e3779b97f4a7c15ULL);
}

bool request_before(const std::string& resource, const ReservationRequest& a, const ReservationRequest& b,
                    bool resource_specific_tie_breaks) {
    if (a.time_s != b.time_s) return a.time_s < b.time_s;
    if (a.priority != b.priority) return a.priority < b.priority;
    if (resource_specific_tie_breaks) {
        const auto a_order = resource_vehicle_order(resource, a.vehicle);
        const auto b_order = resource_vehicle_order(resource, b.vehicle);
        if (a_order != b_order) return a_order < b_order;
    }
    return a.vehicle < b.vehicle;
}

void hash_byte(std::uint64_t& digest, std::uint8_t value) {
    digest ^= value;
    digest *= 1099511628211ULL;
}

void hash_u64(std::uint64_t& digest, std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8)
        hash_byte(digest, static_cast<std::uint8_t>(value >> shift));
}

void hash_double(std::uint64_t& digest, double value) {
    hash_u64(digest, std::bit_cast<std::uint64_t>(value));
}

void hash_string(std::uint64_t& digest, const std::string& value) {
    hash_u64(digest, value.size());
    for (const unsigned char byte : value) hash_byte(digest, byte);
}
}

std::optional<VehicleId> TrafficReservationTable::request_batch(
    std::string resource, std::vector<ReservationRequest> requests) {
    if (resource.empty()) throw std::invalid_argument("reservation resource cannot be empty");
    auto& queue = waiting_[resource];
    for (auto& request : requests) {
        if (request.vehicle.value.empty() || !std::isfinite(request.time_s))
            throw std::invalid_argument("reservation request requires a vehicle ID and finite timestamp");
        if (const auto held = held_.find(resource); held != held_.end() && held->second.owner == request.vehicle)
            continue;
        const auto existing = std::ranges::find(queue, request.vehicle, &ReservationRequest::vehicle);
        if (existing == queue.end()) queue.push_back(std::move(request));
        else if (request_before(resource, request, *existing, resource_specific_tie_breaks_)) *existing = std::move(request);
    }
    if (held_.contains(resource)) return held_.at(resource).owner;
    if (queue.empty()) return std::nullopt;
    std::ranges::sort(queue, [&](const auto& a, const auto& b) { return request_before(resource, a, b, resource_specific_tie_breaks_); });
    auto winner = queue.front();
    queue.erase(queue.begin());
    held_.emplace(resource, Entry{winner.vehicle, winner});
    return winner.vehicle;
}

bool TrafficReservationTable::request(std::string resource, ReservationRequest request) {
    const auto vehicle = request.vehicle;
    return request_batch(std::move(resource), {std::move(request)}) == vehicle;
}

void TrafficReservationTable::retain_waiters(
    const std::string& resource, const std::vector<VehicleId>& active_vehicles) {
    const auto queue = waiting_.find(resource);
    if (queue == waiting_.end()) return;
    std::erase_if(queue->second, [&](const ReservationRequest& request) {
        return std::ranges::find(active_vehicles, request.vehicle) == active_vehicles.end();
    });
    if (queue->second.empty()) waiting_.erase(queue);
}

bool TrafficReservationTable::release(const std::string& resource, const VehicleId& vehicle) {
    const auto held = held_.find(resource);
    if (held == held_.end() || held->second.owner != vehicle) return false;
    held_.erase(held);
    const auto queue = waiting_.find(resource);
    if (queue != waiting_.end() && !queue->second.empty()) {
        std::ranges::sort(queue->second, [&](const auto& a, const auto& b) { return request_before(resource, a, b, resource_specific_tie_breaks_); });
        auto winner = queue->second.front();
        for (auto request = std::next(queue->second.begin()); request != queue->second.end(); ++request) {
            if (request->priority < winner.priority && request->time_s > winner.time_s) {
                ++starvation_preventions_;
                break;
            }
        }
        queue->second.erase(queue->second.begin());
        held_.emplace(resource, Entry{winner.vehicle, winner});
    }
    return true;
}

std::optional<VehicleId> TrafficReservationTable::owner(const std::string& resource) const {
    const auto held = held_.find(resource);
    if (held == held_.end()) return std::nullopt;
    return held->second.owner;
}
std::vector<std::pair<std::string, VehicleId>> TrafficReservationTable::held_resources() const {
    std::vector<std::pair<std::string, VehicleId>> resources;
    resources.reserve(held_.size());
    for (const auto& [resource, entry] : held_) resources.emplace_back(resource, entry.owner);
    std::sort(resources.begin(), resources.end(), [](const auto& left, const auto& right) {
        return left.first < right.first;
    });
    return resources;
}

std::vector<VehicleId> find_deadlocked_vehicles(
    const std::map<VehicleId, std::vector<VehicleId>>& waits_for) {
    std::map<VehicleId, std::set<VehicleId>> reachable;
    for (const auto& [vehicle, blockers] : waits_for) {
        for (const auto& blocker : blockers) {
            if (blocker != vehicle) reachable[vehicle].insert(blocker);
        }
    }
    for (const auto& [vehicle, blockers] : waits_for) {
        (void)blockers;
        reachable.try_emplace(vehicle);
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto& [vehicle, blockers] : reachable) {
            (void)vehicle;
            std::vector<VehicleId> additions;
            for (const auto& blocker : blockers) {
                if (const auto next = reachable.find(blocker); next != reachable.end()) {
                    for (const auto& transitive : next->second) {
                        if (!blockers.contains(transitive)) additions.push_back(transitive);
                    }
                }
            }
            for (const auto& addition : additions) changed = blockers.insert(addition).second || changed;
        }
    }
    std::vector<VehicleId> deadlocked;
    for (const auto& [vehicle, blockers] : reachable) {
        if (blockers.contains(vehicle)) deadlocked.push_back(vehicle);
    }
    return deadlocked;
}

VehicleId choose_recovery_vehicle(std::vector<FleetRecoveryCandidate> candidates) {
    if (candidates.empty()) throw std::invalid_argument("deadlock recovery requires at least one candidate");
    for (const auto& candidate : candidates)
        if (candidate.vehicle.value.empty() || !std::isfinite(candidate.accumulated_wait_s) || candidate.accumulated_wait_s < 0.0)
            throw std::invalid_argument("deadlock recovery candidates require IDs and finite nonnegative wait durations");
    std::ranges::sort(candidates, [](const auto& a, const auto& b) {
        if (a.mission_priority != b.mission_priority) return a.mission_priority > b.mission_priority;
        if (a.accumulated_wait_s != b.accumulated_wait_s) return a.accumulated_wait_s > b.accumulated_wait_s;
        return a.vehicle < b.vehicle;
    });
    return candidates.front().vehicle;
}

struct FleetSimulation::Impl {
    struct OpposingEdgeUse { std::size_t member_index{}; Vec2 entry{}; Vec2 exit{}; double length_m{}; };
    enum class RecoveryState { None, Selected, Retreating, Holding, Resumed, Failed };
    struct Member {
        FleetMission mission;
        std::vector<std::string> capabilities;
        std::string current_node;
        std::optional<ServiceRequestId> current_request;
        std::string service_origin_node;
        std::string service_destination_node;
        std::size_t tasks_completed{};
        double total_distance_m{};
        double busy_time_s{};
        double idle_time_s{};
        double dispatch_state_since_s{};
        bool active{true};
        bool unavailable{};
        bool unavailable_pending{};
        bool unavailable_after_service{};
        bool servicing{};
        double service_complete_at_s{};
        std::vector<std::string> route_resources;
        std::vector<DirectedRouteEdge> route_edges;
        AutonomySimulation simulation;
        ReferenceController controller;
        VehicleCommand command{};
        bool waiting{};
        double wait_s{};
        bool recovery_active{};
        bool route_blocked{};
        RecoveryState recovery_state{RecoveryState::None};
        std::string recovery_resource;
        std::vector<Vec2> traversal_history;
        std::vector<Vec2> retreat_path;
        std::size_t retreat_cursor{1};
        Vec2 retreat_target{};
        double retreat_progress_m{};
        double retreat_last_event_progress_m{};
        double retreat_started_s{};
        double retreat_last_progress_s{};
        Vec2 retreat_last_position{};
        std::size_t recovery_attempts{};
        Member(FleetMission m, AutonomyScenario s, std::vector<std::string> resources,
               std::vector<DirectedRouteEdge> edges, std::uint64_t seed)
            : mission(std::move(m)), route_resources(std::move(resources)), route_edges(std::move(edges)), simulation(std::move(s),seed),
              controller(simulation.scenario().safety_stop_range_m,simulation.scenario().perception_timeout_s,simulation.scenario().localization_timeout_s) {
            traversal_history.push_back(simulation.state().position);
        }
    };
    std::vector<Member> members;
    std::map<std::string, std::vector<std::size_t>> opposing_edge_users;
    std::map<std::string, std::vector<OpposingEdgeUse>> opposing_edge_uses;
    std::vector<TrafficEvent> events;
    std::vector<bool> was_conflicting;
    bool resource_specific_tie_breaks{};
    TrafficReservationTable reservations;
    double now{};
    double minimum_separation{std::numeric_limits<double>::infinity()};
    std::size_t collisions{}, requests{}, contentions{};
    std::size_t deadlock_count{}, near_conflict_events{}, forced_safety_stops{};
    bool opposing_reservations_initialized{};
    std::vector<RoadAvailabilityEvent> road_events;
    std::size_t next_road_event{};
    double deadlock_persistence_s{2.0};
    std::optional<double> cycle_since;
    std::string pending_cycle;
    std::string handled_cycle;
    std::vector<VehicleId> active_cycle;
    std::map<std::pair<VehicleId, std::string>, double> wait_started;
    std::vector<FleetWaitDependency> current_dependencies;
    std::vector<VehicleId> current_deadlocked;
    std::size_t recoveries_resolved{}, recovery_attempts{}, reroutes{}, closure_replans{}, retreats{};
    std::size_t reservation_denials{};
    double resource_wait_total_s{}, resource_wait_max_s{};
    std::size_t resource_wait_samples{};
    AutonomyScenario base_scenario;
    std::unique_ptr<FleetDispatcher> dispatcher;
    bool dispatch_mode{};
    std::uint64_t base_seed{};

    Impl(AutonomyScenario base, std::vector<FleetMission> missions, std::uint64_t seed,
         std::vector<RoadAvailabilityEvent> changes, double persistence, bool resource_tie_breaks,
         std::vector<DispatchVehicle> dispatch_fleet = {}, std::vector<ServiceRequest> service_requests = {},
         double aging_interval_s = 30.0)
        : resource_specific_tie_breaks(resource_tie_breaks), reservations(resource_tie_breaks),
          base_scenario(base), base_seed(seed),
          road_events(std::move(changes)), deadlock_persistence_s(persistence) {
        if (!std::isfinite(deadlock_persistence_s) || deadlock_persistence_s < 0.0)
            throw std::invalid_argument("deadlock persistence threshold must be finite and nonnegative");
        std::ranges::stable_sort(road_events, {}, &RoadAvailabilityEvent::time);
        if (missions.empty()) throw std::invalid_argument("fleet requires at least one mission");
        std::ranges::sort(missions, {}, &FleetMission::id);
        for(std::size_t i=0;i<missions.size();++i) {
            if(missions[i].id.value.empty() || (i && missions[i-1].id==missions[i].id)) throw std::invalid_argument("fleet vehicle IDs must be nonempty and unique");
            auto member_scenario=mission_scenario(base,missions[i]);
            auto resources=route_resources(base,missions[i]);
            auto edges=directed_route_edges(base,missions[i]);
            members.emplace_back(missions[i],std::move(member_scenario),std::move(resources),std::move(edges),seed+static_cast<std::uint64_t>(i)*0x9e3779b97f4a7c15ULL);
        }
        if (!dispatch_fleet.empty() || !service_requests.empty()) {
            if (dispatch_fleet.empty() || dispatch_fleet.size() != members.size())
                throw std::invalid_argument("dispatch fleet requires a matching set of fleet vehicles");
            std::ranges::sort(dispatch_fleet, {}, [](const auto& vehicle) { return vehicle.id; });
            if (dispatch_fleet.size() != members.size()) throw std::invalid_argument("dispatch vehicle and mission counts differ");
            dispatcher = std::make_unique<FleetDispatcher>(std::move(service_requests), aging_interval_s);
            dispatch_mode = true;
            double latest_release = 0.0, maximum_service = 0.0;
            for (const auto& request : dispatcher->snapshot()) {
                latest_release = std::max(latest_release, request.request.release_time_s);
                maximum_service = std::max(maximum_service, request.request.service_duration_s);
            }
            dispatch_horizon_s = latest_release +
                static_cast<double>(dispatcher->snapshot().size() + members.size() + 1) *
                (base_scenario.timeout_s + maximum_service + base_scenario.timestep_s);
            for (std::size_t i = 0; i < members.size(); ++i) {
                auto& member = members[i];
                const auto& vehicle = dispatch_fleet[i];
                if (member.mission.id != vehicle.id) throw std::invalid_argument("dispatch vehicle IDs do not match initialized fleet members");
                if (vehicle.capabilities.empty() || vehicle.current_node != member.mission.start_node)
                    throw std::invalid_argument("dispatch vehicle capabilities or initial node are invalid");
                member.capabilities = vehicle.capabilities;
                member.mission.faults = vehicle.faults;
                member.current_node = vehicle.current_node;
                member.active = false;
                member.mission.goal_node = vehicle.current_node;
            }
        }
        for (std::size_t i = 0; i < missions.size(); ++i) {
            for (std::size_t j = i + 1; j < missions.size(); ++j) {
                for (const auto& resource : opposing_shared_edges(base, missions[i], missions[j])) {
                    auto& users = opposing_edge_users[resource];
                    users.push_back(i);
                    users.push_back(j);
                    for (const auto member_index : {i, j}) {
                        for (const auto& traversal : directed_route_edges(base, missions[member_index])) {
                            if (traversal.resource != resource) continue;
                            const auto entry = base.airport.graph.node(traversal.from).position;
                            const auto exit = base.airport.graph.node(traversal.to).position;
                            auto& uses = opposing_edge_uses[resource];
                            const bool already_added = std::ranges::any_of(uses, [&](const OpposingEdgeUse& use) {
                                return use.member_index == member_index;
                            });
                            if (!already_added) uses.push_back({member_index, entry, exit, separation(entry, exit)});
                        }
                    }
                }
            }
        }
        for (auto& [resource, users] : opposing_edge_users) {
            (void)resource;
            std::ranges::sort(users);
            users.erase(std::unique(users.begin(), users.end()), users.end());
        }
        was_conflicting.resize(members.size()*members.size());
    }

    double dispatch_horizon_s{};

    void extend_dispatch_horizon() {
        if (!dispatcher) return;
        double latest_release = now;
        double maximum_service = 0.0;
        const auto current_requests = dispatcher->snapshot();
        for (const auto& request : current_requests) {
            latest_release = std::max(latest_release, request.request.release_time_s);
            maximum_service = std::max(maximum_service, request.request.service_duration_s);
        }
        dispatch_horizon_s = std::max(dispatch_horizon_s, latest_release +
            static_cast<double>(current_requests.size() + members.size() + 1) *
            (base_scenario.timeout_s + maximum_service + base_scenario.timestep_s));
    }

    [[nodiscard]] std::string nearest_node_name(const Member& member) const {
        const auto graph = member.simulation.graph();
        const auto position = member.simulation.state().position;
        const auto nearest = std::ranges::min_element(graph.nodes(), [&](const auto& a, const auto& b) {
            const double da = separation(position, a.position), db = separation(position, b.position);
            return da == db ? a.id < b.id : da < db;
        });
        if (nearest == graph.nodes().end()) throw std::logic_error("dispatch fleet has no road nodes");
        return nearest->name;
    }

    [[nodiscard]] std::vector<DispatchVehicle> dispatch_vehicle_states() const {
        std::vector<DispatchVehicle> vehicles;
        vehicles.reserve(members.size());
        for (const auto& member : members) {
            DispatchVehicleState state = DispatchVehicleState::Idle;
            if (member.unavailable) state = DispatchVehicleState::Unavailable;
            else if (member.recovery_active || member.recovery_state == RecoveryState::Retreating ||
                     member.recovery_state == RecoveryState::Holding) state = DispatchVehicleState::Recovering;
            else if (member.active) state = DispatchVehicleState::Busy;
            if (member.unavailable || member.unavailable_pending) state = DispatchVehicleState::Unavailable;
            vehicles.push_back({member.mission.id, member.capabilities,
                                member.active ? nearest_node_name(member) : member.current_node,
                                state, member.current_request});
        }
        return vehicles;
    }

    void start_request(Member& member, const ServiceTaskSnapshot& task) {
        member.idle_time_s += std::max(0.0, now - member.dispatch_state_since_s);
        member.dispatch_state_since_s = now;
        const auto graph = member.simulation.graph();
        const auto find_node = [&](const std::string& name) -> std::optional<NodeId> {
            for (const auto& node : graph.nodes()) if (node.name == name) return node.id;
            return std::nullopt;
        };
        const auto from = find_node(member.current_node);
        const auto origin = find_node(task.request.origin);
        const auto destination = find_node(task.request.destination);
        if (!from || !origin || !destination) throw std::invalid_argument("dispatch request references an unknown road node");
        const auto approach = airside::find_route(graph, *from, *origin);
        const auto service_route = airside::find_route(graph, *origin, *destination);
        if (!approach || !service_route) throw std::logic_error("dispatcher assigned a request without an available route");

        auto scenario = member.simulation.scenario();
        scenario.name += "_" + task.request.id.value;
        scenario.start_node = member.current_node;
        scenario.goal_node = task.request.destination;
        scenario.initial_state = member.simulation.state();
        scenario.faults = member.mission.faults;
        scenario.faults.insert(scenario.faults.end(), task.request.faults.begin(), task.request.faults.end());
        member.mission.start_node = member.current_node;
        member.mission.goal_node = task.request.destination;
        member.mission.priority = task.request.priority;
        std::uint64_t task_hash = 14695981039346656037ULL;
        hash_string(task_hash, member.mission.id.value);
        hash_string(task_hash, task.request.id.value);
        member.simulation = AutonomySimulation(std::move(scenario), base_seed ^ task_hash);
        member.controller = ReferenceController(member.simulation.scenario().safety_stop_range_m,
            member.simulation.scenario().perception_timeout_s, member.simulation.scenario().localization_timeout_s);

        std::vector<Vec2> waypoints{member.simulation.state().position};
        const auto append_node = [&](NodeId id) {
            const auto position = graph.node(id).position;
            if (separation(waypoints.back(), position) > 0.05) waypoints.push_back(position);
        };
        for (const auto id : approach->nodes) append_node(id);
        for (std::size_t i = 1; i < service_route->nodes.size(); ++i) append_node(service_route->nodes[i]);
        append_node(*destination);
        member.simulation.set_route(std::move(waypoints));

        member.route_resources.clear();
        member.route_edges.clear();
        const auto append_route = [&](const Route& route) {
            for (std::size_t i = 0; i < route.edges.size(); ++i) {
                const auto edge = route.edges[i];
                const auto resource = "edge/" + std::to_string(edge.value());
                member.route_resources.push_back(resource);
                member.route_edges.push_back({resource, route.nodes[i], route.nodes[i + 1]});
            }
            for (const auto id : route.nodes) member.route_resources.push_back("intersection/" + std::to_string(id.value()));
        };
        append_route(*approach);
        append_route(*service_route);
        std::ranges::sort(member.route_resources);
        member.route_resources.erase(std::unique(member.route_resources.begin(), member.route_resources.end()), member.route_resources.end());
        member.service_origin_node = task.request.origin;
        member.service_destination_node = task.request.destination;
        member.current_request = task.request.id;
        member.active = true;
        member.servicing = false;
        member.service_complete_at_s = 0.0;
        member.waiting = false;
        member.route_blocked = false;
        member.recovery_active = false;
        member.recovery_state = RecoveryState::None;
        member.traversal_history.clear();
        member.traversal_history.push_back(member.simulation.state().position);
        member.retreat_path.clear();
        member.retreat_cursor = 1;
        member.retreat_progress_m = 0.0;
        member.retreat_last_event_progress_m = 0.0;
        member.command = member.controller.update(member.simulation.observe(), member.simulation.mission());
        dispatcher->set_state(task.request.id, ServiceTaskState::EnRoute, now);
    }

    void dispatch_ready_requests() {
        if (!dispatch_mode) return;
        dispatcher->release_due(now);
        const auto graph = members.front().simulation.graph();
        auto vehicles = dispatch_vehicle_states();
        const auto assignments = dispatcher->dispatch(now, vehicles, graph);
        const auto tasks = dispatcher->snapshot();
        for (const auto& [request_id, vehicle_id] : assignments) {
            const auto member = std::ranges::find(members, vehicle_id,
                [](const auto& candidate) { return candidate.mission.id; });
            const auto task = std::ranges::find(tasks, request_id,
                [](const auto& candidate) { return candidate.request.id; });
            if (member == members.end() || task == tasks.end())
                throw std::logic_error("dispatcher assignment references missing fleet state");
            start_request(*member, *task);
        }
        if (!assignments.empty()) rebuild_opposing_route_usage();
    }

    void rebuild_opposing_route_usage() {
        opposing_edge_users.clear();
        opposing_edge_uses.clear();
        for (std::size_t i = 0; i < members.size(); ++i) {
            if (!members[i].active) continue;
            for (std::size_t j = i + 1; j < members.size(); ++j) {
                if (!members[j].active) continue;
                for (const auto& a : members[i].route_edges) {
                    const auto b = std::ranges::find_if(members[j].route_edges, [&](const auto& edge) {
                        return edge.resource == a.resource && edge.from == a.to && edge.to == a.from;
                    });
                    if (b == members[j].route_edges.end()) continue;
                    auto& users = opposing_edge_users[a.resource];
                    users.push_back(i);
                    users.push_back(j);
                    for (const auto index : {i, j}) {
                        const auto& traversal = index == i ? a : *b;
                        const auto graph = members[index].simulation.graph();
                        const auto entry = graph.node(traversal.from).position;
                        const auto exit = graph.node(traversal.to).position;
                        auto& uses = opposing_edge_uses[a.resource];
                        if (std::ranges::none_of(uses, [&](const auto& use) { return use.member_index == index; }))
                            uses.push_back({index, entry, exit, separation(entry, exit)});
                    }
                }
            }
        }
        for (auto& [resource, users] : opposing_edge_users) {
            (void)resource;
            std::ranges::sort(users);
            users.erase(std::unique(users.begin(), users.end()), users.end());
        }
        opposing_reservations_initialized = false;
    }

    void complete_service(Member& member, double completed_at_s) {
        if (!member.current_request) return;
        dispatcher->set_state(*member.current_request, ServiceTaskState::Completed, completed_at_s);
        member.busy_time_s += std::max(0.0, completed_at_s - member.dispatch_state_since_s);
        member.dispatch_state_since_s = completed_at_s;
        ++member.tasks_completed;
        member.current_node = member.service_destination_node;
        member.current_request.reset();
        member.active = false;
        member.servicing = false;
        member.service_complete_at_s = 0.0;
        if (member.unavailable_after_service) {
            member.unavailable = true;
            member.unavailable_after_service = false;
        }
        rebuild_opposing_route_usage();
    }

    void update_dispatch_lifecycle() {
        if (!dispatch_mode) return;
        for (auto& member : members) {
            if (!member.active) continue;
            if (!member.current_request) continue;
            if (member.servicing) {
                if (now + 1e-9 >= member.service_complete_at_s)
                    complete_service(member, now);
                continue;
            }
            if (!member.simulation.finished()) continue;
            const auto run = member.simulation.result();
            member.total_distance_m += run.metrics.distance_traveled_m;
            if (run.metrics.result == MissionResult::Success) {
                const auto task_snapshots = dispatcher->snapshot();
                const auto task = std::ranges::find(task_snapshots, *member.current_request,
                    [](const auto& candidate) { return candidate.request.id; });
                if (task == task_snapshots.end()) throw std::logic_error("completed task disappeared from dispatcher");
                dispatcher->set_state(*member.current_request, ServiceTaskState::Servicing, now);
                member.servicing = true;
                member.service_complete_at_s = now + task->request.service_duration_s;
                if (task->request.service_duration_s <= 0.0) complete_service(member, now);
            } else {
                dispatcher->mark_vehicle_unavailable(member.mission.id, now);
                member.busy_time_s += std::max(0.0, now - member.dispatch_state_since_s);
                member.dispatch_state_since_s = now;
                member.unavailable = true;
                member.active = false;
                member.current_request.reset();
                member.servicing = false;
                member.route_blocked = false;
                member.waiting = false;
                rebuild_opposing_route_usage();
            }
        }
    }

    bool replan(Member& member, std::optional<EdgeId> forbidden_edge = std::nullopt) {
        AirportGraph graph = member.simulation.graph();
        if (forbidden_edge) graph.set_edge_available(*forbidden_edge, false);
        const auto& nodes = graph.nodes();
        if (nodes.empty()) return false;
        const auto nearest = std::ranges::min_element(nodes, [&](const auto& a, const auto& b) {
            const auto pa = member.simulation.state().position;
            const double da = separation(pa, a.position), db = separation(pa, b.position);
            return da == db ? a.id < b.id : da < db;
        });
        std::optional<NodeId> goal;
        for (const auto& node : nodes) if (node.name == member.mission.goal_node) goal = node.id;
        if (!goal) return false;
        NodeId route_start = nearest->id;
        if (forbidden_edge) {
            const auto edge_resource = "edge/" + std::to_string(forbidden_edge->value());
            if (const auto direction = std::ranges::find(member.route_edges, edge_resource, &DirectedRouteEdge::resource);
                direction != member.route_edges.end()) {
                // Backtrack to the approach side of a blocked resource before taking an alternate route.
                route_start = direction->from;
            }
        }
        const auto route = airside::find_route(graph, route_start, *goal);
        if (!route) return false;
        std::vector<Vec2> waypoints{member.simulation.state().position};
        for (const auto node : route->nodes) {
            const auto position = graph.node(node).position;
            if (separation(waypoints.back(), position) > 0.05) waypoints.push_back(position);
        }
        const auto goal_position = graph.node(*goal).position;
        if (waypoints.back() != goal_position) waypoints.push_back(goal_position);
        member.simulation.set_route(std::move(waypoints));
        if (forbidden_edge) member.simulation.set_edge_available(*forbidden_edge, false);
        member.controller = ReferenceController(member.simulation.scenario().safety_stop_range_m,
            member.simulation.scenario().perception_timeout_s, member.simulation.scenario().localization_timeout_s);
        member.command = member.controller.update(member.simulation.observe(), member.simulation.mission());
        member.route_blocked = false;
        const auto replacement = airside::find_route(graph, route_start, *goal);
        member.route_resources.clear();
        member.route_edges.clear();
        if (replacement) {
            for (std::size_t i = 0; i < replacement->edges.size(); ++i) {
                const auto edge = replacement->edges[i];
                member.route_resources.push_back("edge/" + std::to_string(edge.value()));
                member.route_edges.push_back({"edge/" + std::to_string(edge.value()), replacement->nodes[i], replacement->nodes[i + 1]});
            }
            for (const auto node : replacement->nodes) member.route_resources.push_back("intersection/" + std::to_string(node.value()));
        }
        ++reroutes;
        return true;
    }

    bool physically_clear(const Member& member, const std::string& resource) const {
        const auto position = member.simulation.state().position;
        const auto graph = member.simulation.graph();
        const double clearance = member.simulation.scenario().limits.radius_m + 1.0;
        if (resource.starts_with("intersection/")) {
            const auto node = NodeId{static_cast<std::uint32_t>(std::stoul(resource.substr(13)))};
            return separation(position, graph.node(node).position) > clearance;
        }
        if (resource.starts_with("edge/")) {
            const auto edge = EdgeId{static_cast<std::uint32_t>(std::stoul(resource.substr(5)))};
            const auto& road = graph.edge(edge);
            const auto a = graph.node(road.from).position, b = graph.node(road.to).position;
            const double dx = b.x_m - a.x_m, dy = b.y_m - a.y_m;
            const double length_squared = dx * dx + dy * dy;
            const double t = length_squared > 1e-9
                ? std::clamp(((position.x_m - a.x_m) * dx + (position.y_m - a.y_m) * dy) / length_squared, 0.0, 1.0)
                : 0.0;
            return separation(position, {a.x_m + t * dx, a.y_m + t * dy}) > clearance;
        }
        return false;
    }

    bool begin_retreat(Member& member, const std::string& resource) {
        constexpr std::size_t max_attempts = 2;
        if (member.recovery_attempts >= max_attempts || member.traversal_history.size() < 2) return false;
        const auto& state = member.simulation.state();
        if (!(resource.starts_with("edge/") || resource.starts_with("intersection/"))) return false;
        const auto& waypoints = member.simulation.mission().waypoints;
        if (waypoints.size() < 2) return false;
        std::size_t current_segment = 0;
        double best_distance = std::numeric_limits<double>::infinity();
        for (std::size_t i = 0; i + 1 < waypoints.size(); ++i) {
            const double dx = waypoints[i + 1].x_m - waypoints[i].x_m;
            const double dy = waypoints[i + 1].y_m - waypoints[i].y_m;
            const double length_squared = dx * dx + dy * dy;
            const double t = length_squared > 1e-9
                ? std::clamp(((state.position.x_m - waypoints[i].x_m) * dx +
                              (state.position.y_m - waypoints[i].y_m) * dy) / length_squared, 0.0, 1.0)
                : 0.0;
            const double distance_to_segment = separation(state.position,
                {waypoints[i].x_m + t * dx, waypoints[i].y_m + t * dy});
            if (distance_to_segment < best_distance) {
                best_distance = distance_to_segment;
                current_segment = i;
            }
        }
        const Vec2 entry = waypoints[current_segment];
        if (separation(state.position, entry) < 0.75) return false;
        member.retreat_path.clear();
        member.retreat_path.push_back(state.position);
        for (auto it = member.traversal_history.rbegin(); it != member.traversal_history.rend(); ++it) {
            if (separation(member.retreat_path.back(), *it) > 0.05) member.retreat_path.push_back(*it);
            if (separation(*it, entry) <= 0.75) break;
        }
        const auto safe = std::ranges::find_if(member.retreat_path, [&](Vec2 point) {
            return separation(point, entry) <= 0.75;
        });
        if (safe == member.retreat_path.end()) return false;
        member.retreat_path.erase(std::next(safe), member.retreat_path.end());
        member.recovery_state = RecoveryState::Selected;
        member.recovery_resource = resource;
        member.retreat_cursor = member.retreat_path.size() > 1 ? 1 : 0;
        member.retreat_target = member.retreat_path.back();
        member.retreat_progress_m = 0.0;
        member.retreat_last_event_progress_m = 0.0;
        member.retreat_started_s = now;
        member.retreat_last_progress_s = now;
        member.retreat_last_position = state.position;
        ++member.recovery_attempts;
        member.recovery_active = true;
        member.recovery_state = RecoveryState::Retreating;
        events.push_back({now, TrafficEventKind::RetreatSelected, member.mission.id, {}, resource});
        events.push_back({now, TrafficEventKind::RetreatStarted, member.mission.id, {}, resource,
                          state.position, member.retreat_target, 0.0});
        return true;
    }

    VehicleCommand retreat_command(Member& member) {
        constexpr double timeout_s = 60.0;
        constexpr double no_progress_s = 10.0;
        const auto& state = member.simulation.state();
        if (separation(state.position, member.retreat_last_position) >= 0.10) {
            member.retreat_progress_m += separation(state.position, member.retreat_last_position);
            member.retreat_last_position = state.position;
            member.retreat_last_progress_s = now;
            if (member.retreat_progress_m - member.retreat_last_event_progress_m >= 0.5) {
                member.retreat_last_event_progress_m = member.retreat_progress_m;
                events.push_back({now, TrafficEventKind::RetreatProgress, member.mission.id, {}, member.recovery_resource,
                                  state.position, member.retreat_target, member.retreat_progress_m});
            }
        }
        if (now - member.retreat_started_s > timeout_s || now - member.retreat_last_progress_s > no_progress_s) {
            member.recovery_state = RecoveryState::Failed;
            events.push_back({now, TrafficEventKind::RetreatFailed, member.mission.id, {}, member.recovery_resource});
            return {};
        }
        while (member.retreat_cursor < member.retreat_path.size() &&
               separation(state.position, member.retreat_path[member.retreat_cursor]) < 0.35) ++member.retreat_cursor;
        if (member.retreat_cursor >= member.retreat_path.size()) {
            member.recovery_state = RecoveryState::Holding;
            events.push_back({now, TrafficEventKind::RetreatCompleted, member.mission.id, {}, member.recovery_resource,
                              state.position, member.retreat_target, member.retreat_progress_m});
            ++retreats;
            return {};
        }
        const Vec2 previous = member.retreat_path[member.retreat_cursor - 1];
        const Vec2 target = member.retreat_path[member.retreat_cursor];
        const double desired_heading = std::atan2(previous.y_m - target.y_m, previous.x_m - target.x_m);
        const double heading_error = std::remainder(desired_heading - state.heading_rad, 2.0 * std::numbers::pi);
        const double yaw = std::clamp(2.0 * heading_error,
            -member.simulation.scenario().limits.maximum_yaw_rate_radps,
             member.simulation.scenario().limits.maximum_yaw_rate_radps);
        const double speed = std::min(1.0, member.simulation.scenario().limits.maximum_speed_mps * 0.35);
        return {-speed, yaw};
    }
};

FleetSimulation::FleetSimulation(AutonomyScenario b,std::vector<FleetMission> m,std::uint64_t seed,
    std::vector<RoadAvailabilityEvent> road_events,double deadlock_persistence_s,bool resource_tie_breaks)
    :impl_(std::make_unique<Impl>(std::move(b),std::move(m),seed,std::move(road_events),deadlock_persistence_s,resource_tie_breaks)){}
FleetSimulation::FleetSimulation(FleetScenario scenario, std::uint64_t seed)
    : impl_(std::make_unique<Impl>(scenario.vehicle_scenario,
          scenario.dispatch_fleet.empty() ? scenario.missions : idle_missions(scenario.dispatch_fleet),
          seed, scenario.road_events, scenario.deadlock_persistence_s, scenario.resource_specific_tie_breaks,
          scenario.dispatch_fleet, scenario.service_requests, scenario.dispatch_aging_interval_s)) {}
FleetSimulation::~FleetSimulation()=default;
FleetSimulation::FleetSimulation(FleetSimulation&&) noexcept=default;
FleetSimulation& FleetSimulation::operator=(FleetSimulation&&) noexcept=default;
bool FleetSimulation::finished()const noexcept{
    if (impl_->dispatch_mode) return impl_->dispatcher->all_terminal() &&
        std::ranges::none_of(impl_->members, [](const auto& member) { return member.active; });
    return std::ranges::all_of(impl_->members,[](const auto&m){return m.simulation.finished();});
}
double FleetSimulation::time_s()const noexcept{return impl_->now;}
bool FleetSimulation::advance(){
    auto& x=*impl_;
    if (x.dispatch_mode) {
        x.update_dispatch_lifecycle();
        if (x.now + 1e-9 >= x.dispatch_horizon_s && !x.dispatcher->all_terminal())
            x.dispatcher->fail_unfinished(x.now, "dispatch_horizon_exceeded");
    }
    if(finished())return false;
    const double dt=x.base_scenario.timestep_s;
    while (x.next_road_event < x.road_events.size() &&
           static_cast<double>(x.road_events[x.next_road_event].time.count()) <= x.now + 1e-9) {
        const auto change = x.road_events[x.next_road_event++];
        const auto resource = "edge/" + std::to_string(change.edge.value());
        for (auto& member : x.members) member.simulation.set_edge_available(change.edge, change.available);
        x.events.push_back({x.now, change.available ? TrafficEventKind::RoadReopened : TrafficEventKind::RoadClosed, {}, {}, resource});
        for (auto& member : x.members) {
            if (!member.active || member.simulation.finished()) continue;
            const bool route_uses_edge = std::ranges::find(member.route_resources, resource) != member.route_resources.end();
            if ((!change.available && route_uses_edge) || (change.available && member.route_blocked)) {
                if (x.replan(member)) {
                    if (!change.available) ++x.closure_replans;
                    x.events.push_back({x.now, TrafficEventKind::Reroute, member.mission.id, {}, resource});
                } else {
                    member.command.target_speed_mps = 0.0;
                    member.route_blocked = true;
                    x.events.push_back({x.now, TrafficEventKind::Waiting, member.mission.id, {}, resource});
                }
            }
        }
    }
    if (x.dispatch_mode) x.dispatch_ready_requests();
    for(auto& m:x.members) if(m.active&&!m.simulation.finished()) {
        m.command=m.controller.update(m.simulation.observe(),m.simulation.mission());
        if (m.unavailable_pending) m.command.target_speed_mps = 0.0;
    }
    if (!x.opposing_reservations_initialized) {
        for (const auto& [resource, users] : x.opposing_edge_users) {
            std::vector<ReservationRequest> requests;
            requests.reserve(users.size());
            for (const auto index : users) {
                const auto& member = x.members[index];
                requests.push_back({member.mission.id, x.now, member.mission.priority});
            }
            std::ranges::sort(requests, [&](const auto& a, const auto& b) { return request_before(resource, a, b, x.resource_specific_tie_breaks); });
            for (const auto& request : requests)
                x.events.push_back({x.now, TrafficEventKind::Request, request.vehicle, {}, resource});
            x.requests += requests.size();
            const auto owner = x.reservations.request_batch(resource, requests);
            if (owner) {
                x.events.push_back({x.now, TrafficEventKind::Granted, *owner, {}, resource});
                for (const auto& request : requests) if (request.vehicle != *owner) {
                    ++x.contentions;
                    ++x.reservation_denials;
                    x.events.push_back({x.now, TrafficEventKind::Deferred, request.vehicle, *owner, resource});
                }
            }
        }
        x.opposing_reservations_initialized = true;
    }
    struct PairState { std::size_t i{},j{},index{}; bool conflict{},newly_conflicting{}; std::string resource; };
    std::vector<PairState> pairs;
    std::map<std::string,std::vector<ReservationRequest>> requests_by_resource;
    std::set<std::string> newly_conflicting_resources;
    std::vector<bool> yielding(x.members.size());
    std::set<std::string> active_resources;
    std::vector<std::string> release_candidates;
    std::map<VehicleId, std::vector<VehicleId>> waits_for;
    std::map<std::pair<VehicleId, VehicleId>, std::string> wait_resources;
    const auto add_dependency = [&](const VehicleId& waiter, const VehicleId& blocker, const std::string& resource) {
        waits_for[waiter].push_back(blocker);
        wait_resources[{waiter, blocker}] = resource;
    };
    const auto has_safe_recovery_departure = [&](const Impl::Member& recovering, const Impl::Member& other) {
        if (!recovering.recovery_active) return false;
        const auto& own_state = recovering.simulation.estimated_state();
        const auto& other_state = other.simulation.estimated_state();
        const double minimum = recovering.simulation.scenario().limits.radius_m +
            other.simulation.scenario().limits.radius_m + 1.25;
        const double rx = other_state.position.x_m - own_state.position.x_m;
        const double ry = other_state.position.y_m - own_state.position.y_m;
        if (std::hypot(rx, ry) <= minimum + 1.0) return false;
        const double heading = own_state.heading_rad + recovering.command.target_yaw_rate_radps;
        const Vec2 own_velocity{recovering.command.target_speed_mps * std::cos(heading),
                                recovering.command.target_speed_mps * std::sin(heading)};
        const auto other_velocity = velocity(other_state);
        const double vx = other_velocity.x_m - own_velocity.x_m;
        const double vy = other_velocity.y_m - own_velocity.y_m;
        const double horizon = std::clamp(-(rx * vx + ry * vy) / (vx * vx + vy * vy + 1e-9), 0.0, 2.0);
        return std::hypot(rx + vx * horizon, ry + vy * horizon) > minimum + 0.5;
    };
    for(std::size_t i=0;i<x.members.size();++i) for(std::size_t j=i+1;j<x.members.size();++j) {
        auto& a=x.members[i]; auto& b=x.members[j];
        const auto& sa=a.simulation.estimated_state(); const auto& sb=b.simulation.estimated_state();
        const auto va=velocity(sa), vb=velocity(sb);
        const auto index=i*x.members.size()+j;
        const double current=separation(sa.position,sb.position);
        const double rx=sb.position.x_m-sa.position.x_m, ry=sb.position.y_m-sa.position.y_m;
        const double vx=vb.x_m-va.x_m, vy=vb.y_m-va.y_m;
        const double horizon=std::clamp((-(rx*vx+ry*vy))/(vx*vx+vy*vy+1e-9),0.0,4.0);
        const double closest=std::hypot(rx+vx*horizon,ry+vy*horizon);
        const double hard=a.simulation.scenario().limits.radius_m+b.simulation.scenario().limits.radius_m+1.25;
        const bool was_conflicting=x.was_conflicting[index];
        const double activation=was_conflicting?45.0:35.0;
        const bool conflict=current<activation || closest<hard;
        const auto resource=shared_route_resource(a.route_resources,b.route_resources,a.mission.id,b.mission.id);
        pairs.push_back({i,j,index,conflict,conflict&&!was_conflicting,resource});
        const bool pair_active = a.active || b.active;
        if(conflict && pair_active) {
            active_resources.insert(resource);
            auto& candidates=requests_by_resource[resource];
            for(const auto member_index:{i,j}){
                const auto& member=x.members[member_index];
                if (member.recovery_state == Impl::RecoveryState::Retreating ||
                    member.recovery_state == Impl::RecoveryState::Holding) continue;
                if(!was_conflicting)newly_conflicting_resources.insert(resource);
                const int blocked_priority=std::numeric_limits<int>::max();
                const ReservationRequest request{member.mission.id,x.now,
                    (!member.active || member.simulation.finished())?blocked_priority:member.mission.priority};
                const auto existing=std::ranges::find(candidates,request.vehicle,&ReservationRequest::vehicle);
                if(existing==candidates.end())candidates.push_back(request);
                else if(request_before(resource,request,*existing,x.resource_specific_tie_breaks))*existing=request;
            }
        }
        if((!conflict || !pair_active) && x.was_conflicting[index]){
            release_candidates.push_back(resource);
        }
    }
    for(auto& [resource,batch]:requests_by_resource){
        if (x.opposing_edge_users.contains(resource)) continue;
        std::vector<VehicleId> active_vehicles;
        active_vehicles.reserve(batch.size());
        for (const auto& request : batch) active_vehicles.push_back(request.vehicle);
        x.reservations.retain_waiters(resource, active_vehicles);
        if(!newly_conflicting_resources.contains(resource)&&x.reservations.owner(resource))continue;
        std::ranges::sort(batch,[&](const auto& a, const auto& b) { return request_before(resource, a, b, x.resource_specific_tie_breaks); });
        for(const auto& request:batch)x.events.push_back({x.now,TrafficEventKind::Request,request.vehicle,{},resource});
        x.requests+=batch.size();
        const auto owner=x.reservations.request_batch(resource,batch);
        if(owner){
            x.events.push_back({x.now,TrafficEventKind::Granted,*owner,{},resource});
            for(const auto& request:batch)if(request.vehicle!=*owner){
                ++x.contentions;++x.reservation_denials;x.events.push_back({x.now,TrafficEventKind::Deferred,request.vehicle,*owner,resource});
            }
        }
    }
    for (const auto& [resource, owner] : x.reservations.held_resources()) {
        const auto member = std::ranges::find(x.members, owner, [](const auto& candidate) { return candidate.mission.id; });
        if (member == x.members.end() ||
            (member->active && !member->simulation.finished() && member->recovery_state != Impl::RecoveryState::Retreating &&
             member->recovery_state != Impl::RecoveryState::Holding) ||
            !x.physically_clear(*member, resource)) continue;
        if (x.reservations.release(resource, owner)) {
            x.events.push_back({x.now, TrafficEventKind::ReleasedConflict, owner, {}, resource});
            if (const auto next_owner = x.reservations.owner(resource))
                x.events.push_back({x.now, TrafficEventKind::Granted, *next_owner, {}, resource});
        }
    }
    for(const auto& pair:pairs){
        const auto& a=x.members[pair.i];const auto& b=x.members[pair.j];
        if(pair.conflict){
            if(pair.newly_conflicting){++x.near_conflict_events;x.events.push_back({x.now,TrafficEventKind::NearConflict,a.mission.id,b.mission.id,pair.resource});}
            const auto owner=x.reservations.owner(pair.resource);
            const bool recovery_escape_a=has_safe_recovery_departure(a,b);
            const bool recovery_escape_b=has_safe_recovery_departure(b,a);
            if(owner&&pair.newly_conflicting)x.events.push_back({x.now,TrafficEventKind::EnteredConflict,*owner,*owner==a.mission.id?b.mission.id:a.mission.id,pair.resource});
            if(owner&&*owner==a.mission.id){
                if(b.active&&!b.simulation.finished()&&!recovery_escape_b){yielding[pair.j]=true;add_dependency(b.mission.id,a.mission.id,pair.resource);if(!b.waiting){++x.forced_safety_stops;x.events.push_back({x.now,TrafficEventKind::Waiting,b.mission.id,a.mission.id,pair.resource});x.events.push_back({x.now,TrafficEventKind::ForcedSafetyStop,b.mission.id,a.mission.id,pair.resource});}}
            }else if(owner&&*owner==b.mission.id){
                if(a.active&&!a.simulation.finished()&&!recovery_escape_a){yielding[pair.i]=true;add_dependency(a.mission.id,b.mission.id,pair.resource);if(!a.waiting){++x.forced_safety_stops;x.events.push_back({x.now,TrafficEventKind::Waiting,a.mission.id,b.mission.id,pair.resource});x.events.push_back({x.now,TrafficEventKind::ForcedSafetyStop,a.mission.id,b.mission.id,pair.resource});}}
            }else{
                if(a.active&&!a.simulation.finished()&&!recovery_escape_a)yielding[pair.i]=true;
                if(b.active&&!b.simulation.finished()&&!recovery_escape_b)yielding[pair.j]=true;
                if(owner){
                    if(a.active&&!a.simulation.finished()&&!recovery_escape_a)add_dependency(a.mission.id,*owner,pair.resource);
                    if(b.active&&!b.simulation.finished()&&!recovery_escape_b)add_dependency(b.mission.id,*owner,pair.resource);
                }
                if(a.active&&!a.simulation.finished()&&!recovery_escape_a&&!a.waiting){++x.forced_safety_stops;x.events.push_back({x.now,TrafficEventKind::Waiting,a.mission.id,owner.value_or(VehicleId{}),pair.resource});x.events.push_back({x.now,TrafficEventKind::ForcedSafetyStop,a.mission.id,owner.value_or(VehicleId{}),pair.resource});}
                if(b.active&&!b.simulation.finished()&&!recovery_escape_b&&!b.waiting){++x.forced_safety_stops;x.events.push_back({x.now,TrafficEventKind::Waiting,b.mission.id,owner.value_or(VehicleId{}),pair.resource});x.events.push_back({x.now,TrafficEventKind::ForcedSafetyStop,b.mission.id,owner.value_or(VehicleId{}),pair.resource});}
            }
        }
        x.was_conflicting[pair.index]=pair.conflict;
    }
    for (std::size_t i = 0; i < x.members.size(); ++i) {
        auto& member = x.members[i];
        if (!member.recovery_active) continue;
        const bool remains_near = std::ranges::any_of(pairs, [&](const auto& pair) {
            return pair.conflict && (pair.i == i || pair.j == i);
        });
        if (!remains_near) member.recovery_active = false;
    }
    for (const auto& [resource, users] : x.opposing_edge_users) {
        (void)users;
        auto owner = x.reservations.owner(resource);
        if (owner) {
            const auto owner_member = std::ranges::find(
                x.members, *owner, [](const auto& member) { return member.mission.id; });
            bool cleared_edge = false;
            bool owner_finished = false;
            if (owner_member != x.members.end()) {
                owner_finished = !owner_member->active || owner_member->simulation.finished();
                const auto member_index = static_cast<std::size_t>(std::distance(x.members.begin(), owner_member));
                const auto uses = x.opposing_edge_uses.find(resource);
                if (uses != x.opposing_edge_uses.end()) {
                    const auto use = std::ranges::find(uses->second, member_index, &Impl::OpposingEdgeUse::member_index);
                    if (use != uses->second.end()) {
                        const auto position = owner_member->simulation.state().position;
                        const double dx = use->exit.x_m - use->entry.x_m;
                        const double dy = use->exit.y_m - use->entry.y_m;
                        const double progress = ((position.x_m - use->entry.x_m) * dx +
                                                 (position.y_m - use->entry.y_m) * dy) / use->length_m;
                        const double clearance = use->length_m +
                            owner_member->simulation.scenario().limits.radius_m + 1.0;
                        cleared_edge = progress >= clearance;
                    }
                }
            }
            if (cleared_edge && (owner_finished || !active_resources.contains(resource))) {
                (void)x.reservations.release(resource, *owner);
                x.events.push_back({x.now, TrafficEventKind::ReleasedConflict, *owner, {}, resource});
                owner = x.reservations.owner(resource);
                if (owner) x.events.push_back({x.now, TrafficEventKind::Granted, *owner, {}, resource});
            }
        }
        if (!owner) continue;
        for (const auto index : users) {
            auto& member = x.members[index];
            if (!member.active || member.simulation.finished() || member.mission.id == *owner) continue;
            yielding[index] = true;
            add_dependency(member.mission.id,*owner,resource);
            if (!member.waiting) {
                ++x.forced_safety_stops;
                x.events.push_back({x.now, TrafficEventKind::Waiting, member.mission.id, *owner, resource});
                x.events.push_back({x.now, TrafficEventKind::ForcedSafetyStop, member.mission.id, *owner, resource});
            }
        }
    }
    for (std::size_t i = 0; i < x.members.size(); ++i) {
        auto& member = x.members[i];
        if (member.recovery_state == Impl::RecoveryState::Retreating ||
            member.recovery_state == Impl::RecoveryState::Holding) {
            yielding[i] = false;
            waits_for.erase(member.mission.id);
            for (auto& [waiter, blockers] : waits_for) {
                (void)waiter;
                std::erase(blockers, member.mission.id);
            }
        }
    }
    for (std::size_t i = 0; i < x.members.size(); ++i) {
        auto& member = x.members[i];
        if (member.recovery_state != Impl::RecoveryState::Holding) continue;
        const auto graph = member.simulation.graph();
        const auto position = member.simulation.state().position;
        const double clearance = member.simulation.scenario().limits.radius_m + 1.0;
        for (const auto& resource : member.route_resources) {
            const auto owner = x.reservations.owner(resource);
            if (!owner || *owner != member.mission.id) continue;
            double distance_to_resource = std::numeric_limits<double>::infinity();
            if (resource.starts_with("intersection/")) {
                const auto node = NodeId{static_cast<std::uint32_t>(std::stoul(resource.substr(13)))};
                distance_to_resource = separation(position, graph.node(node).position);
            } else if (resource.starts_with("edge/")) {
                const auto edge = EdgeId{static_cast<std::uint32_t>(std::stoul(resource.substr(5)))};
                const auto& road = graph.edge(edge);
                const auto a = graph.node(road.from).position, b = graph.node(road.to).position;
                const double dx = b.x_m - a.x_m, dy = b.y_m - a.y_m;
                const double length_squared = dx * dx + dy * dy;
                const double t = length_squared > 1e-9
                    ? std::clamp(((position.x_m - a.x_m) * dx + (position.y_m - a.y_m) * dy) / length_squared, 0.0, 1.0)
                    : 0.0;
                distance_to_resource = separation(position, {a.x_m + t * dx, a.y_m + t * dy});
            }
            if (distance_to_resource > clearance && x.reservations.release(resource, member.mission.id))
                x.events.push_back({x.now, TrafficEventKind::ReleasedConflict, member.mission.id, {}, resource});
        }
        const auto contested_owner = x.reservations.owner(member.recovery_resource);
        const auto owner_member = contested_owner
            ? std::ranges::find(x.members, *contested_owner, [](const auto& candidate) { return candidate.mission.id; })
            : x.members.end();
        const bool owner_clear = !contested_owner || owner_member == x.members.end() ||
                                 x.physically_clear(*owner_member, member.recovery_resource);
        const bool resource_clear = owner_clear && std::ranges::none_of(pairs, [&](const auto& pair) {
            return pair.conflict && (pair.i == i || pair.j == i) && pair.resource == member.recovery_resource;
        }) && std::abs(member.simulation.state().speed_mps) <= member.simulation.scenario().stopped_speed_mps;
        if (resource_clear) {
            x.events.push_back({x.now, TrafficEventKind::RetreatResourceReleased, member.mission.id, {}, member.recovery_resource});
            member.recovery_state = Impl::RecoveryState::Resumed;
            member.recovery_active = false;
            member.route_blocked = false;
            member.waiting = false;
            (void)x.replan(member);
            x.events.push_back({x.now, TrafficEventKind::MissionResumed, member.mission.id, {}, member.recovery_resource});
        }
    }

    x.current_dependencies.clear();
    std::set<std::pair<VehicleId, std::string>> current_wait_keys;
    for (const auto& [waiter, blockers] : waits_for) for (const auto& blocker : blockers) {
        const auto resource_it = wait_resources.find({waiter, blocker});
        const auto resource = resource_it == wait_resources.end() ? std::string{"unknown"} : resource_it->second;
        const auto key = std::pair{waiter, resource};
        current_wait_keys.insert(key);
        const auto [started, inserted] = x.wait_started.try_emplace(key, x.now);
        (void)inserted;
        const double duration = x.now - started->second;
        x.current_dependencies.push_back({waiter, blocker, resource, duration});
        x.resource_wait_max_s = std::max(x.resource_wait_max_s, duration);
    }
    std::ranges::sort(x.current_dependencies, [](const auto& a, const auto& b) {
        return std::tie(a.waiting_vehicle, a.blocking_vehicle, a.resource) <
               std::tie(b.waiting_vehicle, b.blocking_vehicle, b.resource);
    });
    for (auto it = x.wait_started.begin(); it != x.wait_started.end();) {
        if (!current_wait_keys.contains(it->first)) {
            x.events.push_back({x.now, TrafficEventKind::WaitEnded, it->first.first, {}, it->first.second});
            const double duration = x.now - it->second;
            x.resource_wait_total_s += duration;
            ++x.resource_wait_samples;
            it = x.wait_started.erase(it);
        } else ++it;
    }
    x.current_deadlocked = find_deadlocked_vehicles(waits_for);
    if (x.current_deadlocked.empty()) {
        const bool retreat_in_progress = std::ranges::any_of(x.members, [](const auto& member) {
            return member.recovery_state == Impl::RecoveryState::Retreating ||
                   member.recovery_state == Impl::RecoveryState::Holding;
        });
        if (!x.handled_cycle.empty() && !retreat_in_progress) {
            ++x.recoveries_resolved;
            for (const auto& id : x.active_cycle)
                x.events.push_back({x.now, TrafficEventKind::RecoveryResolved, id, {}, "wait_for_cycle"});
            x.handled_cycle.clear();
            x.active_cycle.clear();
        }
        x.cycle_since.reset();
        x.pending_cycle.clear();
    } else {
        std::string cycle_key;
        for (const auto& id : x.current_deadlocked) cycle_key += id.value + ";";
        if (!x.cycle_since || cycle_key != x.pending_cycle) {
            x.cycle_since = x.now;
            x.pending_cycle = cycle_key;
        }
        if (x.now - *x.cycle_since + 1e-9 >= x.deadlock_persistence_s && cycle_key != x.handled_cycle) {
            ++x.deadlock_count;
            x.handled_cycle = cycle_key;
            x.active_cycle = x.current_deadlocked;
            for (const auto& id : x.current_deadlocked)
                x.events.push_back({x.now, TrafficEventKind::DeadlockDetected, id, {}, "wait_for_cycle"});
            std::vector<std::size_t> candidates;
            for (std::size_t i = 0; i < x.members.size(); ++i)
                if (std::ranges::find(x.current_deadlocked, x.members[i].mission.id) != x.current_deadlocked.end()) candidates.push_back(i);
            std::vector<FleetRecoveryCandidate> selection;
            for (const auto index : candidates) selection.push_back({x.members[index].mission.id,x.members[index].mission.priority,x.members[index].wait_s});
            if (!selection.empty()) {
                const auto yield_id = choose_recovery_vehicle(std::move(selection));
                const auto candidate = std::ranges::find(x.members,yield_id,[](const auto& member){return member.mission.id;});
                const auto candidate_index = static_cast<std::size_t>(std::distance(x.members.begin(),candidate));
                auto& yield_member = *candidate;
                std::string contested_resource;
                for (const auto& dependency : x.current_dependencies) {
                    if (dependency.waiting_vehicle != yield_member.mission.id) continue;
                    try {
                        if (dependency.resource.starts_with("edge/")) {
                            contested_resource = dependency.resource;
                            break;
                        }
                        if (dependency.resource.starts_with("intersection/")) {
                            contested_resource = dependency.resource;
                            break;
                        }
                    } catch (...) { }
                }
                ++x.recovery_attempts;
                x.events.push_back({x.now, TrafficEventKind::DeadlockRecovery, yield_member.mission.id, {}, "deterministic_opposing_road_retreat"});
                if (!contested_resource.empty() && x.begin_retreat(yield_member, contested_resource)) {
                    yielding[candidate_index] = false;
                } else {
                    yield_member.command.target_speed_mps = 0.0;
                    yield_member.waiting = true;
                    yield_member.route_blocked = true;
                    yield_member.recovery_state = Impl::RecoveryState::Failed;
                    x.events.push_back({x.now, TrafficEventKind::RetreatFailed, yield_member.mission.id, {}, contested_resource});
                }
            }
        }
    }
    std::ranges::sort(release_candidates);
    release_candidates.erase(std::unique(release_candidates.begin(),release_candidates.end()),release_candidates.end());
    for(const auto& resource:release_candidates)if(!active_resources.contains(resource)){
        if (x.opposing_edge_users.contains(resource)) continue;
        x.reservations.retain_waiters(resource, {});
        const auto owner=x.reservations.owner(resource);
        if(owner){(void)x.reservations.release(resource,*owner);x.events.push_back({x.now,TrafficEventKind::ReleasedConflict,*owner,{},resource});}
    }
    for(std::size_t i=0;i<x.members.size();++i) {
        auto& m=x.members[i]; if(!m.active||m.simulation.finished())continue;
        const bool retreating = m.recovery_state == Impl::RecoveryState::Retreating;
        const bool holding = m.recovery_state == Impl::RecoveryState::Holding;
        if (retreating) m.command = x.retreat_command(m);
        if(yielding[i]||m.route_blocked||holding) {m.command.target_speed_mps=0.0;m.wait_s+=dt;}
        m.waiting=yielding[i]||m.route_blocked;
        if (retreating || holding) (void)m.simulation.advance_with_reverse_command(m.command,m.controller);
        else (void)m.simulation.advance_with_command(m.command,m.controller);
        if (m.unavailable_pending &&
            std::abs(m.simulation.state().speed_mps) <= m.simulation.scenario().stopped_speed_mps) {
            x.dispatcher->mark_vehicle_unavailable(m.mission.id, x.now + dt);
            m.busy_time_s += std::max(0.0, x.now + dt - m.dispatch_state_since_s);
            m.dispatch_state_since_s = x.now + dt;
            m.unavailable = true;
            m.unavailable_pending = false;
            m.current_request.reset();
            m.active = false;
            m.servicing = false;
            m.waiting = false;
            m.route_blocked = false;
            x.rebuild_opposing_route_usage();
        }
        if (m.recovery_state == Impl::RecoveryState::None || m.recovery_state == Impl::RecoveryState::Resumed) {
            const auto position = m.simulation.state().position;
            if (separation(m.traversal_history.back(), position) >= 0.20) m.traversal_history.push_back(position);
        }
    }
    x.now+=dt;
    if (x.dispatch_mode) x.update_dispatch_lifecycle();
    for(std::size_t i=0;i<x.members.size();++i)for(std::size_t j=i+1;j<x.members.size();++j){
        const auto&a=x.members[i].simulation.state();const auto&b=x.members[j].simulation.state();
        const double d=separation(a.position,b.position);x.minimum_separation=std::min(x.minimum_separation,d);
        if(d<x.members[i].simulation.scenario().limits.radius_m+x.members[j].simulation.scenario().limits.radius_m){++x.collisions;x.events.push_back({x.now,TrafficEventKind::Collision,x.members[i].mission.id,x.members[j].mission.id,"vehicle_conflict",a.position,b.position});}
    }
    return !finished();
}
void FleetSimulation::add_service_request(ServiceRequest request) {
    if (!impl_->dispatch_mode || !impl_->dispatcher)
        throw std::logic_error("fleet simulation is not configured for service dispatch");
    impl_->dispatcher->add_request(std::move(request));
    impl_->extend_dispatch_horizon();
}
void FleetSimulation::update_service_duration(const ServiceRequestId& request, double duration_s) {
    if (!impl_->dispatch_mode || !impl_->dispatcher)
        throw std::logic_error("fleet simulation is not configured for service dispatch");
    impl_->dispatcher->update_service_duration(request, duration_s, impl_->now);
    impl_->extend_dispatch_horizon();
}
void FleetSimulation::add_road_event(RoadAvailabilityEvent event) {
    if (event.time < SimTime::zero() || static_cast<double>(event.time.count()) + 1e-9 < impl_->now)
        throw std::invalid_argument("fleet road event must be scheduled at or after current simulation time");
    impl_->road_events.push_back(event);
    std::ranges::stable_sort(impl_->road_events.begin() + static_cast<std::ptrdiff_t>(impl_->next_road_event),
        impl_->road_events.end(), {}, &RoadAvailabilityEvent::time);
}
void FleetSimulation::mark_vehicle_unavailable(const VehicleId& id) {
    if (!impl_->dispatch_mode || !impl_->dispatcher)
        throw std::logic_error("fleet simulation is not configured for service dispatch");
    const auto found = std::ranges::find(impl_->members, id, [](const auto& member) { return member.mission.id; });
    if (found == impl_->members.end()) throw std::out_of_range("unknown fleet vehicle: " + id.value);
    if (found->unavailable || found->unavailable_pending || found->unavailable_after_service) return;
    if (found->servicing) {
        found->unavailable_after_service = true;
        return;
    }
    if (found->active) {
        found->unavailable_pending = true;
        found->command.target_speed_mps = 0.0;
        return;
    }
    impl_->dispatcher->mark_vehicle_unavailable(id, impl_->now);
    found->unavailable = true;
}
std::vector<ServiceTaskSnapshot> FleetSimulation::service_requests() const {
    return impl_->dispatcher ? impl_->dispatcher->snapshot() : std::vector<ServiceTaskSnapshot>{};
}
FleetDispatchMetrics FleetSimulation::dispatch_metrics() const {
    return impl_->dispatcher ? impl_->dispatcher->metrics() : FleetDispatchMetrics{};
}
FleetMetrics FleetSimulation::result()const{
    const auto&x=*impl_;FleetMetrics r;r.vehicle_count=x.members.size();r.collisions=x.collisions;r.minimum_separation_m=std::isfinite(x.minimum_separation)?x.minimum_separation:0.0;r.reservation_requests=x.requests;r.reservation_contentions=x.contentions;r.deadlock_count=x.deadlock_count;r.deadlocks_resolved=x.recoveries_resolved;r.recovery_attempts=x.recovery_attempts;r.reroutes=x.reroutes;r.road_closure_replans=x.closure_replans;r.retreat_count=x.retreats;r.reservation_denials=x.reservation_denials;r.outstanding_reservations=static_cast<std::size_t>(std::ranges::count_if(x.reservations.held_resources(),[&](const auto& held){const auto member=std::ranges::find(x.members,held.second,[](const auto& value){return value.mission.id;});return member!=x.members.end()&&member->active;}));r.starvation_preventions=x.reservations.starvation_preventions();r.maximum_resource_wait_s=x.resource_wait_max_s;r.mean_resource_wait_s=x.resource_wait_samples?x.resource_wait_total_s/static_cast<double>(x.resource_wait_samples):0.0;r.wait_dependencies=x.current_dependencies;r.deadlocked_vehicles=x.current_deadlocked;r.near_conflict_events=x.near_conflict_events;r.forced_safety_stops=x.forced_safety_stops;r.events=x.events;r.deterministic_digest=14695981039346656037ULL;
    if (x.dispatch_mode) {
        r.dispatch = x.dispatcher->metrics();
        r.missions_attempted = r.dispatch.requests_created;
        r.missions_completed = r.dispatch.requests_completed;
        r.safe_timeouts = r.dispatch.requests_failed;
        r.cumulative_waiting_time_s = r.dispatch.total_queue_wait_s;
        r.total_mission_time_s = r.dispatch.maximum_completion_time_s;
        for (const auto& member : x.members) {
            auto run = member.simulation.result();
            const auto distance_m = member.total_distance_m +
                (member.active && !member.servicing ? run.metrics.distance_traveled_m : 0.0);
            r.vehicles.push_back({member.mission.id, member.mission.start_node, member.mission.goal_node,
                                  run.metrics, run.final_state, member.tasks_completed, distance_m,
                                  member.busy_time_s + (member.active ? std::max(0.0, x.now - member.dispatch_state_since_s) : 0.0),
                                  member.idle_time_s + (!member.active && !member.unavailable ? std::max(0.0, x.now - member.dispatch_state_since_s) : 0.0),
                                  x.now > 0.0 ? (member.busy_time_s + (member.active ? std::max(0.0, x.now - member.dispatch_state_since_s) : 0.0)) / x.now : 0.0});
            r.total_distance_m += distance_m;
            r.traffic_waiting_time_s += member.wait_s;
            r.safety_stop_time_s += run.metrics.time_stopped_degraded_s;
        }
    } else {
        for(const auto&m:x.members){const auto run=m.simulation.result();r.vehicles.push_back({m.mission.id,m.mission.start_node,m.mission.goal_node,run.metrics,run.final_state,0,run.metrics.distance_traveled_m});++r.missions_attempted;if(run.metrics.result==MissionResult::Success)++r.missions_completed;else if(run.metrics.result==MissionResult::Timeout)++r.safe_timeouts;r.total_distance_m+=run.metrics.distance_traveled_m;r.total_mission_time_s=std::max(r.total_mission_time_s,run.metrics.completion_time_s);r.cumulative_waiting_time_s+=m.wait_s;r.traffic_waiting_time_s+=m.wait_s;r.safety_stop_time_s+=run.metrics.time_stopped_degraded_s;}
    }
    r.throughput_per_simulated_hour=x.now>0.0?static_cast<double>(r.missions_completed)*3600.0/x.now:0.0;
    auto& digest = r.deterministic_digest;
    hash_u64(digest, r.vehicle_count); hash_u64(digest, r.missions_attempted);
    hash_u64(digest, r.missions_completed); hash_u64(digest, r.safe_timeouts);
    hash_u64(digest, r.collisions); hash_double(digest, r.minimum_separation_m);
    hash_double(digest, r.total_distance_m); hash_double(digest, r.total_mission_time_s);
    hash_double(digest, r.cumulative_waiting_time_s); hash_double(digest, r.traffic_waiting_time_s);
    hash_double(digest, r.safety_stop_time_s); hash_u64(digest, r.reservation_requests);
    hash_u64(digest, r.reservation_contentions); hash_u64(digest, r.deadlock_count);
    hash_u64(digest, r.deadlocks_resolved); hash_u64(digest, r.recovery_attempts);
    hash_u64(digest, r.reroutes); hash_u64(digest, r.road_closure_replans);
    hash_u64(digest, r.retreat_count); hash_u64(digest, r.reservation_denials);
    hash_u64(digest, r.outstanding_reservations);
    hash_u64(digest, r.starvation_preventions); hash_double(digest, r.maximum_resource_wait_s);
    hash_double(digest, r.mean_resource_wait_s);
    hash_u64(digest, r.near_conflict_events); hash_u64(digest, r.forced_safety_stops);
    hash_double(digest, r.throughput_per_simulated_hour);
    for (const auto& vehicle : r.vehicles) {
        hash_string(digest, vehicle.id.value); hash_string(digest, vehicle.start_node);
        hash_string(digest, vehicle.goal_node);
        hash_u64(digest, vehicle.metrics.trajectory_digest);
        hash_u64(digest, vehicle.metrics.sensor_stream_digest);
        hash_double(digest, vehicle.final_state.position.x_m);
        hash_double(digest, vehicle.final_state.position.y_m);
        hash_double(digest, vehicle.final_state.heading_rad);
        hash_double(digest, vehicle.final_state.speed_mps);
        hash_double(digest, vehicle.final_state.distance_m);
        hash_double(digest, vehicle.busy_time_s); hash_double(digest, vehicle.idle_time_s);
        hash_double(digest, vehicle.utilization);
    }
    for (const auto& event : x.events) {
        hash_double(digest, event.time_s); hash_u64(digest, static_cast<std::uint64_t>(event.kind));
        hash_string(digest, event.vehicle.value); hash_string(digest, event.other.value);
        hash_string(digest, event.resource);
        hash_double(digest, event.position.x_m); hash_double(digest, event.position.y_m);
        hash_double(digest, event.target.x_m); hash_double(digest, event.target.y_m);
        hash_double(digest, event.progress_m);
    }
    if (x.dispatch_mode) {
        for (const auto& request : r.dispatch.requests) {
            hash_string(digest, request.request.id.value);
            hash_u64(digest, static_cast<std::uint64_t>(request.state));
            hash_string(digest, request.assigned_vehicle ? request.assigned_vehicle->value : std::string{});
            hash_double(digest, request.released_at_s);
            hash_double(digest, request.assigned_at_s);
            hash_double(digest, request.completed_at_s);
            hash_double(digest, request.queue_wait_s);
            hash_u64(digest, request.reassignments);
        }
        for (const auto& event : r.dispatch.events) {
            hash_double(digest, event.time_s);
            hash_u64(digest, static_cast<std::uint64_t>(event.kind));
            hash_string(digest, event.request.value);
            hash_string(digest, event.vehicle.value);
            hash_string(digest, event.detail);
            hash_u64(digest, static_cast<std::uint64_t>(event.effective_priority));
            hash_double(digest, event.route_distance_m);
        }
    }
    return r;
}
std::vector<FleetVehicleSnapshot> FleetSimulation::snapshots()const{
    std::vector<FleetVehicleSnapshot> out;out.reserve(impl_->members.size());
    for(const auto&m:impl_->members){
        std::string state;
        switch(m.recovery_state){
        case Impl::RecoveryState::None: state=m.waiting?"waiting":"normal";break;
        case Impl::RecoveryState::Selected: state="selected";break;
        case Impl::RecoveryState::Retreating: state="retreating";break;
        case Impl::RecoveryState::Holding: state="holding";break;
        case Impl::RecoveryState::Resumed: state="resumed";break;
        case Impl::RecoveryState::Failed: state="failed";break;
        }
        auto autonomy = m.simulation.snapshot();
        if (impl_->dispatch_mode && !m.active) autonomy.finished = true;
        const auto dispatch_state = m.unavailable ? DispatchVehicleState::Unavailable :
            m.recovery_active ? DispatchVehicleState::Recovering :
            m.active ? DispatchVehicleState::Busy : DispatchVehicleState::Idle;
        out.push_back({m.mission.id,m.mission.goal_node,m.current_request,dispatch_state,m.capabilities,m.waiting,
                       std::move(autonomy),std::move(state),m.recovery_resource,m.retreat_target,
                       m.retreat_progress_m,m.recovery_attempts,m.tasks_completed,m.total_distance_m});
    }
    return out;
}
std::vector<FleetWaitDependency> FleetSimulation::wait_dependencies()const{return impl_->current_dependencies;}
std::vector<VehicleId> FleetSimulation::deadlocked_vehicles()const{return impl_->current_deadlocked;}
std::string to_string(TrafficEventKind k){switch(k){case TrafficEventKind::Request:return"request";case TrafficEventKind::Granted:return"granted";case TrafficEventKind::Deferred:return"deferred";case TrafficEventKind::Waiting:return"fleet_wait_started";case TrafficEventKind::WaitEnded:return"fleet_wait_ended";case TrafficEventKind::EnteredConflict:return"entered_conflict";case TrafficEventKind::ReleasedConflict:return"released_conflict";case TrafficEventKind::Collision:return"collision";case TrafficEventKind::DeadlockDetected:return"fleet_deadlock_detected";case TrafficEventKind::DeadlockRecovery:return"fleet_deadlock_resolution_started";case TrafficEventKind::RecoveryResolved:return"fleet_deadlock_resolved";case TrafficEventKind::Retreat:return"retreat";case TrafficEventKind::RetreatSelected:return"retreat_selected";case TrafficEventKind::RetreatStarted:return"retreat_started";case TrafficEventKind::RetreatProgress:return"retreat_progress";case TrafficEventKind::RetreatResourceReleased:return"retreat_resource_released";case TrafficEventKind::RetreatCompleted:return"retreat_completed";case TrafficEventKind::MissionResumed:return"mission_resumed";case TrafficEventKind::RetreatFailed:return"retreat_failed";case TrafficEventKind::Reroute:return"reroute";case TrafficEventKind::RoadClosed:return"road_closed";case TrafficEventKind::RoadReopened:return"road_reopened";case TrafficEventKind::NearConflict:return"near_conflict";case TrafficEventKind::ForcedSafetyStop:return"forced_safety_stop";}return"unknown";}
} // namespace airside::autonomy
