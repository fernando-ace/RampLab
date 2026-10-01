#include "airside/autonomy/fleet.hpp"
#include "airside/routing/astar.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <ranges>
#include <set>

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

std::string shared_route_resource(const std::vector<std::string>& a,const std::vector<std::string>& b,
                                 const VehicleId& a_id,const VehicleId& b_id) {
    for(const auto& key:a)if(key.starts_with("edge/")&&std::ranges::find(b,key)!=b.end())return key;
    for(const auto& key:a)if(key.starts_with("intersection/")&&std::ranges::find(b,key)!=b.end())return key;
    return "vehicle_conflict/"+a_id.value+"/"+b_id.value;
}

}

namespace {
bool request_before(const ReservationRequest& a, const ReservationRequest& b) {
    if (a.time_s != b.time_s) return a.time_s < b.time_s;
    if (a.priority != b.priority) return a.priority < b.priority;
    return a.vehicle < b.vehicle;
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
        else if (request_before(request, *existing)) *existing = std::move(request);
    }
    if (held_.contains(resource)) return held_.at(resource).owner;
    if (queue.empty()) return std::nullopt;
    std::ranges::sort(queue, request_before);
    auto winner = queue.front();
    queue.erase(queue.begin());
    held_.emplace(resource, Entry{winner.vehicle, winner});
    return winner.vehicle;
}

bool TrafficReservationTable::request(std::string resource, ReservationRequest request) {
    const auto vehicle = request.vehicle;
    return request_batch(std::move(resource), {std::move(request)}) == vehicle;
}

bool TrafficReservationTable::release(const std::string& resource, const VehicleId& vehicle) {
    const auto held = held_.find(resource);
    if (held == held_.end() || held->second.owner != vehicle) return false;
    held_.erase(held);
    const auto queue = waiting_.find(resource);
    if (queue != waiting_.end() && !queue->second.empty()) {
        std::ranges::sort(queue->second, request_before);
        auto winner = queue->second.front();
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

struct FleetSimulation::Impl {
    struct Member {
        FleetMission mission;
        std::vector<std::string> route_resources;
        AutonomySimulation simulation;
        ReferenceController controller;
        VehicleCommand command{};
        bool waiting{};
        bool deadlock_stopped{};
        double wait_s{};
        Member(FleetMission m, AutonomyScenario s, std::vector<std::string> resources,std::uint64_t seed)
            : mission(std::move(m)), route_resources(std::move(resources)), simulation(std::move(s),seed),
              controller(simulation.scenario().safety_stop_range_m,simulation.scenario().perception_timeout_s,simulation.scenario().localization_timeout_s) {}
    };
    std::vector<Member> members;
    std::vector<TrafficEvent> events;
    std::vector<bool> was_conflicting;
    TrafficReservationTable reservations;
    double now{};
    double minimum_separation{std::numeric_limits<double>::infinity()};
    std::size_t collisions{}, requests{}, contentions{};
    std::size_t deadlock_count{}, near_conflict_events{}, forced_safety_stops{};

    Impl(AutonomyScenario base, std::vector<FleetMission> missions, std::uint64_t seed) {
        if (missions.empty()) throw std::invalid_argument("fleet requires at least one mission");
        std::ranges::sort(missions, {}, &FleetMission::id);
        for(std::size_t i=0;i<missions.size();++i) {
            if(missions[i].id.value.empty() || (i && missions[i-1].id==missions[i].id)) throw std::invalid_argument("fleet vehicle IDs must be nonempty and unique");
            auto member_scenario=mission_scenario(base,missions[i]);
            auto resources=route_resources(base,missions[i]);
            members.emplace_back(missions[i],std::move(member_scenario),std::move(resources),seed+static_cast<std::uint64_t>(i)*0x9e3779b97f4a7c15ULL);
        }
        was_conflicting.resize(members.size()*members.size());
    }
};

FleetSimulation::FleetSimulation(AutonomyScenario b,std::vector<FleetMission> m,std::uint64_t seed):impl_(std::make_unique<Impl>(std::move(b),std::move(m),seed)){}
FleetSimulation::~FleetSimulation()=default;
FleetSimulation::FleetSimulation(FleetSimulation&&) noexcept=default;
FleetSimulation& FleetSimulation::operator=(FleetSimulation&&) noexcept=default;
bool FleetSimulation::finished()const noexcept{return std::ranges::all_of(impl_->members,[](const auto&m){return m.simulation.finished();});}
double FleetSimulation::time_s()const noexcept{return impl_->now;}
bool FleetSimulation::advance(){
    auto& x=*impl_; if(finished())return false;
    const double dt=x.members.front().simulation.scenario().timestep_s;
    for(auto& m:x.members) if(!m.simulation.finished()) m.command=m.controller.update(m.simulation.observe(),m.simulation.mission());
    struct PairState { std::size_t i{},j{},index{}; bool conflict{},newly_conflicting{}; std::string resource; };
    std::vector<PairState> pairs;
    std::map<std::string,std::vector<ReservationRequest>> requests_by_resource;
    std::set<std::string> newly_conflicting_resources;
    std::vector<bool> yielding(x.members.size());
    std::set<std::string> active_resources;
    std::vector<std::string> release_candidates;
    std::map<VehicleId, std::vector<VehicleId>> waits_for;
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
        if(conflict) {
            active_resources.insert(resource);
            auto& candidates=requests_by_resource[resource];
            for(const auto member_index:{i,j}){
                const auto& member=x.members[member_index];
                if(!was_conflicting)newly_conflicting_resources.insert(resource);
                const int blocked_priority=std::numeric_limits<int>::min();
                const ReservationRequest request{member.mission.id,x.now,
                    member.simulation.finished()?blocked_priority:member.mission.priority};
                const auto existing=std::ranges::find(candidates,request.vehicle,&ReservationRequest::vehicle);
                if(existing==candidates.end())candidates.push_back(request);
                else if(request_before(request,*existing))*existing=request;
            }
        }
        if(!conflict && x.was_conflicting[index]){
            release_candidates.push_back(resource);
        }
    }
    for(auto& [resource,batch]:requests_by_resource){
        if(!newly_conflicting_resources.contains(resource)&&x.reservations.owner(resource))continue;
        std::ranges::sort(batch,request_before);
        for(const auto& request:batch)x.events.push_back({x.now,TrafficEventKind::Request,request.vehicle,{},resource});
        x.requests+=batch.size();
        const auto owner=x.reservations.request_batch(resource,batch);
        if(owner){
            x.events.push_back({x.now,TrafficEventKind::Granted,*owner,{},resource});
            for(const auto& request:batch)if(request.vehicle!=*owner){
                ++x.contentions;x.events.push_back({x.now,TrafficEventKind::Deferred,request.vehicle,*owner,resource});
            }
        }
    }
    for(const auto& pair:pairs){
        const auto& a=x.members[pair.i];const auto& b=x.members[pair.j];
        if(pair.conflict){
            if(pair.newly_conflicting){++x.near_conflict_events;x.events.push_back({x.now,TrafficEventKind::NearConflict,a.mission.id,b.mission.id,pair.resource});}
            const auto owner=x.reservations.owner(pair.resource);
            if(owner&&pair.newly_conflicting)x.events.push_back({x.now,TrafficEventKind::EnteredConflict,*owner,*owner==a.mission.id?b.mission.id:a.mission.id,pair.resource});
            if(owner&&*owner==a.mission.id){
                if(!b.simulation.finished()){yielding[pair.j]=true;waits_for[b.mission.id].push_back(a.mission.id);if(!b.waiting){++x.forced_safety_stops;x.events.push_back({x.now,TrafficEventKind::Waiting,b.mission.id,a.mission.id,pair.resource});x.events.push_back({x.now,TrafficEventKind::ForcedSafetyStop,b.mission.id,a.mission.id,pair.resource});}}
            }else if(owner&&*owner==b.mission.id){
                if(!a.simulation.finished()){yielding[pair.i]=true;waits_for[a.mission.id].push_back(b.mission.id);if(!a.waiting){++x.forced_safety_stops;x.events.push_back({x.now,TrafficEventKind::Waiting,a.mission.id,b.mission.id,pair.resource});x.events.push_back({x.now,TrafficEventKind::ForcedSafetyStop,a.mission.id,b.mission.id,pair.resource});}}
            }else{
                if(!a.simulation.finished())yielding[pair.i]=true;
                if(!b.simulation.finished())yielding[pair.j]=true;
                if(owner){
                    if(!a.simulation.finished())waits_for[a.mission.id].push_back(*owner);
                    if(!b.simulation.finished())waits_for[b.mission.id].push_back(*owner);
                }
                if(!a.simulation.finished()&&!a.waiting){++x.forced_safety_stops;x.events.push_back({x.now,TrafficEventKind::Waiting,a.mission.id,owner.value_or(VehicleId{}),pair.resource});x.events.push_back({x.now,TrafficEventKind::ForcedSafetyStop,a.mission.id,owner.value_or(VehicleId{}),pair.resource});}
                if(!b.simulation.finished()&&!b.waiting){++x.forced_safety_stops;x.events.push_back({x.now,TrafficEventKind::Waiting,b.mission.id,owner.value_or(VehicleId{}),pair.resource});x.events.push_back({x.now,TrafficEventKind::ForcedSafetyStop,b.mission.id,owner.value_or(VehicleId{}),pair.resource});}
            }
        }
        x.was_conflicting[pair.index]=pair.conflict;
    }
    const auto deadlocked=find_deadlocked_vehicles(waits_for);
    if(!deadlocked.empty()){
        std::vector<std::size_t> newly_deadlocked;
        for(const auto& id:deadlocked){
            const auto member=std::ranges::find(x.members,id,[](const auto& item){return item.mission.id;});
            if(member!=x.members.end()&&!member->deadlock_stopped)newly_deadlocked.push_back(static_cast<std::size_t>(std::distance(x.members.begin(),member)));
        }
        if(!newly_deadlocked.empty()){
            ++x.deadlock_count;
            for(const auto i:newly_deadlocked){
                auto& member=x.members[i];member.deadlock_stopped=true;yielding[i]=true;
                x.events.push_back({x.now,TrafficEventKind::DeadlockDetected,member.mission.id,{},"wait_for_cycle"});
                x.events.push_back({x.now,TrafficEventKind::DeadlockRecovery,member.mission.id,{},"safe_stop_until_timeout"});
            }
        }
    }
    std::ranges::sort(release_candidates);
    release_candidates.erase(std::unique(release_candidates.begin(),release_candidates.end()),release_candidates.end());
    for(const auto& resource:release_candidates)if(!active_resources.contains(resource)){
        const auto owner=x.reservations.owner(resource);
        if(owner){(void)x.reservations.release(resource,*owner);x.events.push_back({x.now,TrafficEventKind::ReleasedConflict,*owner,{},resource});}
    }
    for(std::size_t i=0;i<x.members.size();++i) {
        auto& m=x.members[i]; if(m.simulation.finished())continue;
        if(yielding[i]||m.deadlock_stopped) {m.command.target_speed_mps=0.0;m.wait_s+=dt;}
        m.waiting=yielding[i]||m.deadlock_stopped;
        (void)m.simulation.advance_with_command(m.command,m.controller);
    }
    x.now+=dt;
    for(std::size_t i=0;i<x.members.size();++i)for(std::size_t j=i+1;j<x.members.size();++j){
        const auto&a=x.members[i].simulation.state();const auto&b=x.members[j].simulation.state();
        const double d=separation(a.position,b.position);x.minimum_separation=std::min(x.minimum_separation,d);
        if(d<x.members[i].simulation.scenario().limits.radius_m+x.members[j].simulation.scenario().limits.radius_m){++x.collisions;x.events.push_back({x.now,TrafficEventKind::Collision,x.members[i].mission.id,x.members[j].mission.id,"vehicle_conflict"});}
    }
    return !finished();
}
FleetMetrics FleetSimulation::result()const{
    const auto&x=*impl_;FleetMetrics r;r.vehicle_count=x.members.size();r.collisions=x.collisions;r.minimum_separation_m=std::isfinite(x.minimum_separation)?x.minimum_separation:0.0;r.reservation_requests=x.requests;r.reservation_contentions=x.contentions;r.deadlock_count=x.deadlock_count;r.near_conflict_events=x.near_conflict_events;r.forced_safety_stops=x.forced_safety_stops;r.events=x.events;r.deterministic_digest=14695981039346656037ULL;
    for(const auto&m:x.members){const auto run=m.simulation.result();r.vehicles.push_back({m.mission.id,m.mission.start_node,m.mission.goal_node,run.metrics,run.final_state});++r.missions_attempted;if(run.metrics.result==MissionResult::Success)++r.missions_completed;else if(run.metrics.result==MissionResult::Timeout)++r.safe_timeouts;r.total_distance_m+=run.metrics.distance_traveled_m;r.total_mission_time_s=std::max(r.total_mission_time_s,run.metrics.completion_time_s);r.cumulative_waiting_time_s+=m.wait_s;r.traffic_waiting_time_s+=m.wait_s;r.safety_stop_time_s+=run.metrics.time_stopped_degraded_s;}
    r.throughput_per_simulated_hour=x.now>0.0?static_cast<double>(r.missions_completed)*3600.0/x.now:0.0;
    for(const auto&e:x.events){for(const auto c:e.vehicle.value) {r.deterministic_digest^=static_cast<unsigned char>(c);r.deterministic_digest*=1099511628211ULL;}r.deterministic_digest^=static_cast<std::uint64_t>(e.kind);r.deterministic_digest*=1099511628211ULL;}
    return r;
}
std::vector<FleetVehicleSnapshot> FleetSimulation::snapshots()const{std::vector<FleetVehicleSnapshot> out;out.reserve(impl_->members.size());for(const auto&m:impl_->members)out.push_back({m.mission.id,m.mission.goal_node,m.waiting,m.simulation.snapshot()});return out;}
std::string to_string(TrafficEventKind k){switch(k){case TrafficEventKind::Request:return"request";case TrafficEventKind::Granted:return"granted";case TrafficEventKind::Deferred:return"deferred";case TrafficEventKind::Waiting:return"waiting";case TrafficEventKind::EnteredConflict:return"entered_conflict";case TrafficEventKind::ReleasedConflict:return"released_conflict";case TrafficEventKind::Collision:return"collision";case TrafficEventKind::DeadlockDetected:return"deadlock_detected";case TrafficEventKind::DeadlockRecovery:return"deadlock_recovery";case TrafficEventKind::NearConflict:return"near_conflict";case TrafficEventKind::ForcedSafetyStop:return"forced_safety_stop";}return"unknown";}
} // namespace airside::autonomy
