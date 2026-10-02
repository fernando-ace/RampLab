#include "airside/autonomy/fleet.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string_view>

namespace {
std::string csv_quote(std::string_view value) {
    std::string result="\"";
    for(const char c:value){if(c=='\"')result+="\"\"";else result+=c;}
    result+='\"';return result;
}
std::string json_quote(std::string_view value) {
    std::ostringstream out;out<<'\"';
    for(const unsigned char c:value){
        switch(c){case '\"':out<<"\\\"";break;case '\\':out<<"\\\\";break;case '\b':out<<"\\b";break;
        case '\f':out<<"\\f";break;case '\n':out<<"\\n";break;case '\r':out<<"\\r";break;case '\t':out<<"\\t";break;
        default:if(c<0x20)out<<"\\u00"<<std::hex<<std::setw(2)<<std::setfill('0')<<static_cast<unsigned>(c)<<std::dec;else out<<static_cast<char>(c);}
    }
    out<<'\"';return out.str();
}
void write_csv(const std::filesystem::path& path,std::string_view scenario,std::uint64_t seed,const airside::autonomy::FleetMetrics& r,double sim_time_s){
    std::ofstream out(path);if(!out)throw std::runtime_error("cannot open fleet CSV output: "+path.string());
    const std::vector<std::string> header={"record_type","scenario","seed","vehicle_id","start_node","goal_node","result","vehicle_count","missions_attempted","missions_completed","safe_timeouts","collisions","minimum_separation_m","total_distance_m","total_mission_time_s","cumulative_waiting_time_s","traffic_waiting_time_s","safety_stop_time_s","reservation_requests","reservation_contentions","deadlocks","deadlocks_resolved","recovery_attempts","retreats","reroutes","road_closure_replans","maximum_resource_wait_s","mean_resource_wait_s","near_conflict_events","forced_safety_stops","throughput_per_simulated_hour","completion_time_s","distance_m","final_x_m","final_y_m","final_speed_mps","reservation_denials","starvation_preventions","outstanding_reservations","requests_created","requests_completed","requests_failed","assignments","reassignments","aging_activations","average_queue_wait_s","maximum_queue_wait_s","unfinished_requests","tasks_completed","request_id","request_type","required_capability","priority","release_time_s","assigned_vehicle","assigned_at_s","completed_at_s","queue_wait_s","task_state","busy_time_s","idle_time_s","utilization","simulation_time_s"};
    const auto row=[&](const std::vector<std::string>& values){for(std::size_t i=0;i<values.size();++i){if(i)out<<',';out<<csv_quote(values[i]);}out<<'\n';};
    const auto n=[](auto value){std::ostringstream cell;cell<<std::setprecision(17)<<value;return cell.str();};
    row(header);
    std::vector<std::string> v(header.size());v[0]="fleet";v[1]=scenario;v[2]=n(seed);
    v[7]=n(r.vehicle_count);v[8]=n(r.missions_attempted);v[9]=n(r.missions_completed);v[10]=n(r.safe_timeouts);v[11]=n(r.collisions);v[12]=n(r.minimum_separation_m);v[13]=n(r.total_distance_m);v[14]=n(r.total_mission_time_s);v[15]=n(r.cumulative_waiting_time_s);v[16]=n(r.traffic_waiting_time_s);v[17]=n(r.safety_stop_time_s);v[18]=n(r.reservation_requests);v[19]=n(r.reservation_contentions);v[20]=n(r.deadlock_count);v[21]=n(r.deadlocks_resolved);v[22]=n(r.recovery_attempts);v[23]=n(r.retreat_count);v[24]=n(r.reroutes);v[25]=n(r.road_closure_replans);v[26]=n(r.maximum_resource_wait_s);v[27]=n(r.mean_resource_wait_s);v[28]=n(r.near_conflict_events);v[29]=n(r.forced_safety_stops);v[30]=n(sim_time_s>0.0?static_cast<double>(r.missions_completed)*3600.0/sim_time_s:0.0);v[36]=n(r.reservation_denials);v[37]=n(r.starvation_preventions);v[38]=n(r.outstanding_reservations);v[39]=n(r.dispatch.requests_created);v[40]=n(r.dispatch.requests_completed);v[41]=n(r.dispatch.requests_failed);v[42]=n(r.dispatch.assignments);v[43]=n(r.dispatch.reassignments);v[44]=n(r.dispatch.aging_activations);v[45]=n(r.dispatch.average_queue_wait_s);v[46]=n(r.dispatch.maximum_queue_wait_s);v[47]=n(r.dispatch.unfinished_requests);v[62]=n(sim_time_s);row(v);
    for(const auto& item:r.vehicles){v.assign(header.size(),{});v[0]="vehicle";v[1]=scenario;v[2]=n(seed);v[3]=item.id.value;v[4]=item.start_node;v[5]=item.goal_node;v[6]=airside::autonomy::to_string(item.metrics.result);v[31]=n(item.metrics.completion_time_s);v[32]=n(item.total_distance_m);v[33]=n(item.final_state.position.x_m);v[34]=n(item.final_state.position.y_m);v[35]=n(item.final_state.speed_mps);v[48]=n(item.tasks_completed);v[59]=n(item.busy_time_s);v[60]=n(item.idle_time_s);v[61]=n(item.utilization);row(v);}
    for(const auto& task:r.dispatch.requests){v.assign(header.size(),{});v[0]="request";v[1]=scenario;v[2]=n(seed);v[3]=task.request.id.value;v[4]=task.request.origin;v[5]=task.request.destination;v[6]=airside::autonomy::to_string(task.state);v[39]=n(r.dispatch.requests_created);v[40]=n(r.dispatch.requests_completed);v[41]=n(r.dispatch.requests_failed);v[43]=n(task.reassignments);v[49]=task.request.id.value;v[50]=airside::autonomy::to_string(task.request.kind);v[51]=task.request.required_capability;v[52]=n(task.request.priority);v[53]=n(task.request.release_time_s);v[54]=task.assigned_vehicle?task.assigned_vehicle->value:"";v[55]=n(task.assigned_at_s);v[56]=n(task.completed_at_s);v[57]=n(task.queue_wait_s);v[58]=airside::autonomy::to_string(task.state);row(v);}
    if(!out)throw std::runtime_error("failed writing fleet CSV output: "+path.string());
}
void write_json(const std::filesystem::path& path,std::string_view scenario,std::uint64_t seed,const airside::autonomy::FleetMetrics& r,double sim_time_s){
    std::ofstream out(path);if(!out)throw std::runtime_error("cannot open fleet JSON output: "+path.string());
    out<<std::setprecision(17)<<"{\"scenario\":"<<json_quote(scenario)<<",\"seed\":"<<seed<<",\"fleet\":{\"vehicle_count\":"<<r.vehicle_count
       <<",\"missions_attempted\":"<<r.missions_attempted<<",\"missions_completed\":"<<r.missions_completed<<",\"safe_timeouts\":"<<r.safe_timeouts
       <<",\"simulation_time_s\":"<<sim_time_s<<",\"collisions\":"<<r.collisions<<",\"minimum_separation_m\":"<<r.minimum_separation_m<<",\"total_distance_m\":"<<r.total_distance_m
       <<",\"total_mission_time_s\":"<<r.total_mission_time_s<<",\"cumulative_waiting_time_s\":"<<r.cumulative_waiting_time_s
       <<",\"traffic_waiting_time_s\":"<<r.traffic_waiting_time_s<<",\"safety_stop_time_s\":"<<r.safety_stop_time_s
       <<",\"reservation_requests\":"<<r.reservation_requests<<",\"reservation_contentions\":"<<r.reservation_contentions<<",\"reservation_denials\":"<<r.reservation_denials<<",\"starvation_preventions\":"<<r.starvation_preventions<<",\"deadlock_count\":"<<r.deadlock_count<<",\"deadlocks_resolved\":"<<r.deadlocks_resolved<<",\"recovery_attempts\":"<<r.recovery_attempts<<",\"retreats\":"<<r.retreat_count<<",\"reroutes\":"<<r.reroutes<<",\"road_closure_replans\":"<<r.road_closure_replans<<",\"maximum_resource_wait_s\":"<<r.maximum_resource_wait_s<<",\"mean_resource_wait_s\":"<<r.mean_resource_wait_s<<",\"near_conflict_events\":"<<r.near_conflict_events<<",\"forced_safety_stops\":"<<r.forced_safety_stops
       <<",\"outstanding_reservations\":"<<r.outstanding_reservations
       <<",\"throughput_per_simulated_hour\":"<<(sim_time_s>0.0?static_cast<double>(r.missions_completed)*3600.0/sim_time_s:0.0)<<",\"deterministic_digest\":"<<r.deterministic_digest
       <<"},\"dispatch\":{\"requests_created\":"<<r.dispatch.requests_created<<",\"requests_completed\":"<<r.dispatch.requests_completed
       <<",\"requests_failed\":"<<r.dispatch.requests_failed<<",\"deadline_misses\":"<<r.dispatch.deadline_misses
       <<",\"assignments\":"<<r.dispatch.assignments<<",\"reassignments\":"<<r.dispatch.reassignments
       <<",\"aging_activations\":"<<r.dispatch.aging_activations<<",\"total_queue_wait_s\":"<<r.dispatch.total_queue_wait_s
       <<",\"average_queue_wait_s\":"<<r.dispatch.average_queue_wait_s<<",\"maximum_queue_wait_s\":"<<r.dispatch.maximum_queue_wait_s
       <<",\"maximum_completion_time_s\":"<<r.dispatch.maximum_completion_time_s<<",\"unfinished_requests\":"<<r.dispatch.unfinished_requests<<",\"requests\":[";
    for(std::size_t i=0;i<r.dispatch.requests.size();++i){const auto& t=r.dispatch.requests[i];if(i)out<<',';
        out<<"{\"id\":"<<json_quote(t.request.id.value)<<",\"type\":"<<json_quote(airside::autonomy::to_string(t.request.kind))
           <<",\"required_capability\":"<<json_quote(t.request.required_capability)<<",\"origin\":"<<json_quote(t.request.origin)
           <<",\"destination\":"<<json_quote(t.request.destination)<<",\"priority\":"<<t.request.priority
           <<",\"release_time_s\":"<<t.request.release_time_s<<",\"state\":"<<json_quote(airside::autonomy::to_string(t.state))
           <<",\"assigned_vehicle\":"<<(t.assigned_vehicle?json_quote(t.assigned_vehicle->value):"null")
           <<",\"assigned_at_s\":"<<t.assigned_at_s<<",\"completed_at_s\":"<<t.completed_at_s
           <<",\"queue_wait_s\":"<<t.queue_wait_s<<",\"reassignments\":"<<t.reassignments
           <<",\"effective_priority\":"<<t.effective_priority<<'}';}
    out<<"],\"events\":[";
    for(std::size_t i=0;i<r.dispatch.events.size();++i){const auto& e=r.dispatch.events[i];if(i)out<<',';
        out<<"{\"time_s\":"<<e.time_s<<",\"kind\":"<<json_quote(airside::autonomy::to_string(e.kind))
           <<",\"request_id\":"<<json_quote(e.request.value)<<",\"vehicle_id\":"<<json_quote(e.vehicle.value)
           <<",\"detail\":"<<json_quote(e.detail)<<",\"effective_priority\":"<<e.effective_priority
           <<",\"route_distance_m\":"<<e.route_distance_m<<'}';}
    out<<"]},\"vehicles\":[";
    for(std::size_t i=0;i<r.vehicles.size();++i){const auto& v=r.vehicles[i];if(i)out<<',';
        out<<"{\"id\":"<<json_quote(v.id.value)<<",\"start_node\":"<<json_quote(v.start_node)<<",\"goal_node\":"<<json_quote(v.goal_node)
           <<",\"result\":"<<json_quote(airside::autonomy::to_string(v.metrics.result))<<",\"completion_time_s\":"<<v.metrics.completion_time_s
           <<",\"distance_m\":"<<v.total_distance_m<<",\"tasks_completed\":"<<v.tasks_completed
           <<",\"busy_time_s\":"<<v.busy_time_s<<",\"idle_time_s\":"<<v.idle_time_s<<",\"utilization\":"<<v.utilization
           <<",\"degraded_mode_entries\":"<<v.metrics.degraded_mode_entries
           <<",\"final_state\":{\"x_m\":"<<v.final_state.position.x_m<<",\"y_m\":"<<v.final_state.position.y_m<<",\"heading_rad\":"<<v.final_state.heading_rad<<",\"speed_mps\":"<<v.final_state.speed_mps<<"}}";}
    out<<"],\"events\":[";
    for(std::size_t i=0;i<r.events.size();++i){const auto& e=r.events[i];if(i)out<<',';
        out<<"{\"time_s\":"<<e.time_s<<",\"kind\":"<<json_quote(airside::autonomy::to_string(e.kind))<<",\"vehicle\":"<<json_quote(e.vehicle.value)
           <<",\"other\":"<<json_quote(e.other.value)<<",\"resource\":"<<json_quote(e.resource)
           <<",\"x_m\":"<<e.position.x_m<<",\"y_m\":"<<e.position.y_m<<",\"target_x_m\":"<<e.target.x_m
           <<",\"target_y_m\":"<<e.target.y_m<<",\"progress_m\":"<<e.progress_m<<'}';}
    out<<"]}\n";if(!out)throw std::runtime_error("failed writing fleet JSON output: "+path.string());
}
}

int main(int argc,char** argv){
    try{
        std::filesystem::path path="scenarios/autonomy_fleet.yaml",csv_path,json_path;std::uint64_t seed=0;
        for(int i=1;i<argc;++i){const std::string_view arg=argv[i];if(arg=="--scenario"&&i+1<argc)path=argv[++i];else if(arg=="--seed"&&i+1<argc)seed=std::stoull(argv[++i]);else if(arg=="--csv"&&i+1<argc)csv_path=argv[++i];else if(arg=="--json"&&i+1<argc)json_path=argv[++i];else if(arg=="--help"){std::cout<<"airside_fleet --scenario FILE [--seed N] [--csv FILE] [--json FILE]\n";return 0;}else throw std::runtime_error("unknown or incomplete option");}
        auto s=airside::autonomy::load_fleet_scenario(path);if(!seed)seed=s.default_seed;
        airside::autonomy::FleetSimulation fleet{s,seed};
        const auto begin=std::chrono::steady_clock::now();std::size_t steps=0;
        while(fleet.advance())++steps;
        const auto wall=std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();const auto r=fleet.result();
        if(!csv_path.empty())write_csv(csv_path,s.name,seed,r,fleet.time_s());
        if(!json_path.empty())write_json(json_path,s.name,seed,r,fleet.time_s());
        std::cout<<std::setprecision(9)<<"scenario="<<s.name<<" seed="<<seed<<" vehicles="<<r.vehicle_count<<" sim_time_s="<<fleet.time_s()<<" steps="<<steps<<" wall_s="<<wall<<" missions="<<r.missions_completed<<'/'<<r.missions_attempted<<" safe_timeouts="<<r.safe_timeouts<<" collisions="<<r.collisions<<" min_separation_m="<<r.minimum_separation_m<<" distance_m="<<r.total_distance_m<<" waiting_s="<<r.traffic_waiting_time_s<<" reservations="<<r.reservation_requests<<" contention="<<r.reservation_contentions<<" outstanding_reservations="<<r.outstanding_reservations<<" near_conflicts="<<r.near_conflict_events<<" forced_safety_stops="<<r.forced_safety_stops<<" deadlocks="<<r.deadlock_count<<" recoveries="<<r.recovery_attempts<<" retreats="<<r.retreat_count<<" reroutes="<<r.reroutes<<" digest="<<r.deterministic_digest<<'\n';
        const auto snapshots=fleet.snapshots();
        for(std::size_t i=0;i<r.vehicles.size();++i){const auto& v=r.vehicles[i];const auto& state=snapshots[i];std::cout<<v.id.value<<','<<v.start_node<<" -> "<<v.goal_node<<','<<airside::autonomy::to_string(v.metrics.result)<<','<<v.metrics.completion_time_s<<','<<v.metrics.distance_traveled_m<<','<<v.metrics.degraded_mode_entries<<",recovery="<<state.recovery_state<<",resource="<<state.recovery_resource<<",position="<<v.final_state.position.x_m<<':'<<v.final_state.position.y_m<<",retreat_progress_m="<<state.retreat_progress_m<<",speed_mps="<<v.final_state.speed_mps<<'\n';}
        for(const auto& wait:r.wait_dependencies)std::cout<<"wait,"<<wait.waiting_vehicle.value<<','<<wait.blocking_vehicle.value<<','<<wait.resource<<','<<wait.wait_duration_s<<'\n';
        for(const auto&e:r.events)std::cout<<"event,"<<e.time_s<<','<<airside::autonomy::to_string(e.kind)<<','<<e.vehicle.value<<','<<e.other.value<<','<<e.resource<<'\n';
        return r.collisions?2:0;
    }catch(const std::exception&e){std::cerr<<"airside_fleet: "<<e.what()<<'\n';return 1;}
}
