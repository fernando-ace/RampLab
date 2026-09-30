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
    out<<"record_type,scenario,seed,vehicle_id,start_node,goal_node,result,vehicle_count,missions_attempted,missions_completed,safe_timeouts,collisions,minimum_separation_m,total_distance_m,total_mission_time_s,cumulative_waiting_time_s,traffic_waiting_time_s,safety_stop_time_s,reservation_requests,reservation_contentions,deadlocks,throughput_per_simulated_hour,completion_time_s,distance_m\n";
    out<<std::setprecision(17)<<"fleet,"<<csv_quote(scenario)<<','<<seed<<",,,,,"<<r.vehicle_count<<','<<r.missions_attempted<<','<<r.missions_completed<<','<<r.safe_timeouts<<','<<r.collisions<<','<<r.minimum_separation_m<<','<<r.total_distance_m<<','<<r.total_mission_time_s<<','<<r.cumulative_waiting_time_s<<','<<r.traffic_waiting_time_s<<','<<r.safety_stop_time_s<<','<<r.reservation_requests<<','<<r.reservation_contentions<<','<<r.deadlock_count<<','<<(sim_time_s>0.0?static_cast<double>(r.missions_completed)*3600.0/sim_time_s:0.0)<<",,\n";
    for(const auto& v:r.vehicles)out<<"vehicle,"<<csv_quote(scenario)<<','<<seed<<','<<csv_quote(v.id.value)<<','<<csv_quote(v.start_node)<<','<<csv_quote(v.goal_node)<<','<<airside::autonomy::to_string(v.metrics.result)<<",,,,,,,,,,,,,,,,"<<v.metrics.completion_time_s<<','<<v.metrics.distance_traveled_m<<'\n';
    if(!out)throw std::runtime_error("failed writing fleet CSV output: "+path.string());
}
void write_json(const std::filesystem::path& path,std::string_view scenario,std::uint64_t seed,const airside::autonomy::FleetMetrics& r,double sim_time_s){
    std::ofstream out(path);if(!out)throw std::runtime_error("cannot open fleet JSON output: "+path.string());
    out<<std::setprecision(17)<<"{\"scenario\":"<<json_quote(scenario)<<",\"seed\":"<<seed<<",\"fleet\":{\"vehicle_count\":"<<r.vehicle_count
       <<",\"missions_attempted\":"<<r.missions_attempted<<",\"missions_completed\":"<<r.missions_completed<<",\"safe_timeouts\":"<<r.safe_timeouts
       <<",\"collisions\":"<<r.collisions<<",\"minimum_separation_m\":"<<r.minimum_separation_m<<",\"total_distance_m\":"<<r.total_distance_m
       <<",\"total_mission_time_s\":"<<r.total_mission_time_s<<",\"cumulative_waiting_time_s\":"<<r.cumulative_waiting_time_s
       <<",\"traffic_waiting_time_s\":"<<r.traffic_waiting_time_s<<",\"safety_stop_time_s\":"<<r.safety_stop_time_s
       <<",\"reservation_requests\":"<<r.reservation_requests<<",\"reservation_contentions\":"<<r.reservation_contentions<<",\"deadlock_count\":"<<r.deadlock_count
       <<",\"throughput_per_simulated_hour\":"<<(sim_time_s>0.0?static_cast<double>(r.missions_completed)*3600.0/sim_time_s:0.0)<<",\"deterministic_digest\":"<<r.deterministic_digest<<"},\"vehicles\":[";
    for(std::size_t i=0;i<r.vehicles.size();++i){const auto& v=r.vehicles[i];if(i)out<<',';
        out<<"{\"id\":"<<json_quote(v.id.value)<<",\"start_node\":"<<json_quote(v.start_node)<<",\"goal_node\":"<<json_quote(v.goal_node)
           <<",\"result\":"<<json_quote(airside::autonomy::to_string(v.metrics.result))<<",\"completion_time_s\":"<<v.metrics.completion_time_s
           <<",\"distance_m\":"<<v.metrics.distance_traveled_m<<",\"degraded_mode_entries\":"<<v.metrics.degraded_mode_entries<<'}';}
    out<<"],\"events\":[";
    for(std::size_t i=0;i<r.events.size();++i){const auto& e=r.events[i];if(i)out<<',';
        out<<"{\"time_s\":"<<e.time_s<<",\"kind\":"<<json_quote(airside::autonomy::to_string(e.kind))<<",\"vehicle\":"<<json_quote(e.vehicle.value)
           <<",\"other\":"<<json_quote(e.other.value)<<",\"resource\":"<<json_quote(e.resource)<<'}';}
    out<<"]}\n";if(!out)throw std::runtime_error("failed writing fleet JSON output: "+path.string());
}
}

int main(int argc,char** argv){
    try{
        std::filesystem::path path="scenarios/autonomy_fleet.yaml",csv_path,json_path;std::uint64_t seed=0;
        for(int i=1;i<argc;++i){const std::string_view arg=argv[i];if(arg=="--scenario"&&i+1<argc)path=argv[++i];else if(arg=="--seed"&&i+1<argc)seed=std::stoull(argv[++i]);else if(arg=="--csv"&&i+1<argc)csv_path=argv[++i];else if(arg=="--json"&&i+1<argc)json_path=argv[++i];else if(arg=="--help"){std::cout<<"airside_fleet --scenario FILE [--seed N] [--csv FILE] [--json FILE]\n";return 0;}else throw std::runtime_error("unknown or incomplete option");}
        auto s=airside::autonomy::load_fleet_scenario(path);if(!seed)seed=s.default_seed;
        airside::autonomy::FleetSimulation fleet{s.vehicle_scenario,s.missions,seed};
        const auto begin=std::chrono::steady_clock::now();std::size_t steps=0;
        while(fleet.advance())++steps;
        const auto wall=std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();const auto r=fleet.result();
        if(!csv_path.empty())write_csv(csv_path,s.name,seed,r,fleet.time_s());
        if(!json_path.empty())write_json(json_path,s.name,seed,r,fleet.time_s());
        std::cout<<std::setprecision(9)<<"scenario="<<s.name<<" seed="<<seed<<" vehicles="<<r.vehicle_count<<" sim_time_s="<<fleet.time_s()<<" steps="<<steps<<" wall_s="<<wall<<" missions="<<r.missions_completed<<'/'<<r.missions_attempted<<" safe_timeouts="<<r.safe_timeouts<<" collisions="<<r.collisions<<" min_separation_m="<<r.minimum_separation_m<<" distance_m="<<r.total_distance_m<<" waiting_s="<<r.traffic_waiting_time_s<<" reservations="<<r.reservation_requests<<" contention="<<r.reservation_contentions<<" deadlocks="<<r.deadlock_count<<" digest="<<r.deterministic_digest<<'\n';
        for(const auto& v:r.vehicles)std::cout<<v.id.value<<','<<v.start_node<<" -> "<<v.goal_node<<','<<airside::autonomy::to_string(v.metrics.result)<<','<<v.metrics.completion_time_s<<','<<v.metrics.distance_traveled_m<<','<<v.metrics.degraded_mode_entries<<'\n';
        for(const auto&e:r.events)std::cout<<"event,"<<e.time_s<<','<<airside::autonomy::to_string(e.kind)<<','<<e.vehicle.value<<','<<e.other.value<<','<<e.resource<<'\n';
        return r.collisions?2:0;
    }catch(const std::exception&e){std::cerr<<"airside_fleet: "<<e.what()<<'\n';return 1;}
}
