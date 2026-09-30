#include "airside/autonomy/fleet.hpp"

#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string_view>

int main(int argc,char** argv){
    try{
        std::filesystem::path path="scenarios/autonomy_fleet.yaml";std::uint64_t seed=0;
        for(int i=1;i<argc;++i){const std::string_view arg=argv[i];if(arg=="--scenario"&&i+1<argc)path=argv[++i];else if(arg=="--seed"&&i+1<argc)seed=std::stoull(argv[++i]);else if(arg=="--help"){std::cout<<"airside_fleet --scenario FILE [--seed N]\n";return 0;}else throw std::runtime_error("unknown or incomplete option");}
        auto s=airside::autonomy::load_fleet_scenario(path);if(!seed)seed=s.default_seed;
        airside::autonomy::FleetSimulation fleet{s.vehicle_scenario,s.missions,seed};
        const auto begin=std::chrono::steady_clock::now();std::size_t steps=0;
        while(fleet.advance())++steps;
        const auto wall=std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();const auto r=fleet.result();
        std::cout<<std::setprecision(9)<<"scenario="<<s.name<<" seed="<<seed<<" vehicles="<<r.vehicle_count<<" sim_time_s="<<fleet.time_s()<<" steps="<<steps<<" wall_s="<<wall<<" missions="<<r.missions_completed<<'/'<<r.missions_attempted<<" safe_timeouts="<<r.safe_timeouts<<" collisions="<<r.collisions<<" min_separation_m="<<r.minimum_separation_m<<" distance_m="<<r.total_distance_m<<" waiting_s="<<r.traffic_waiting_time_s<<" reservations="<<r.reservation_requests<<" contention="<<r.reservation_contentions<<" digest="<<r.deterministic_digest<<'\n';
        for(const auto& v:r.vehicles)std::cout<<v.id.value<<','<<v.start_node<<" -> "<<v.goal_node<<','<<airside::autonomy::to_string(v.metrics.result)<<','<<v.metrics.completion_time_s<<','<<v.metrics.distance_traveled_m<<','<<v.metrics.degraded_mode_entries<<'\n';
        for(const auto&e:r.events)std::cout<<"event,"<<e.time_s<<','<<airside::autonomy::to_string(e.kind)<<','<<e.vehicle.value<<','<<e.other.value<<','<<e.resource<<'\n';
        return r.collisions?2:0;
    }catch(const std::exception&e){std::cerr<<"airside_fleet: "<<e.what()<<'\n';return 1;}
}
