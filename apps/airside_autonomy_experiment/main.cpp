#include "airside/autonomy/experiment.hpp"
#include "airside/autonomy/scenario_loader.hpp"
#include <yaml-cpp/yaml.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string_view>
#include <vector>
namespace {
struct Options{std::filesystem::path file="experiments/autonomy_noise_validation.yaml";std::size_t workers{};};
Options parse(int argc,char**argv){Options o;for(int i=1;i<argc;++i){std::string_view a=argv[i];if(a=="--experiment"&&i+1<argc)o.file=argv[++i];else if(a=="--workers"&&i+1<argc)o.workers=static_cast<std::size_t>(std::stoull(argv[++i]));else if(a=="--help"){std::cout<<"airside_autonomy_experiment --experiment FILE [--workers N]\n";std::exit(0);}else throw std::runtime_error("unknown or incomplete option: "+std::string(a));}return o;}
double mean(const std::vector<double>&v){return v.empty()?0.0:std::accumulate(v.begin(),v.end(),0.0)/static_cast<double>(v.size());}
double p95(std::vector<double> v){if(v.empty())return 0;std::ranges::sort(v);return v[static_cast<std::size_t>(std::ceil(.95*static_cast<double>(v.size())))-1];}
}
int main(int argc,char**argv){try{const auto o=parse(argc,argv);const auto root=YAML::LoadFile(o.file.string());const auto base=o.file.parent_path();auto scenario=airside::autonomy::load_scenario(base/root["scenario"].as<std::string>());const auto count=root["runs_per_noise_level"].as<std::size_t>();const auto first=root["seed_start"].as<std::uint64_t>();const auto workers=o.workers?o.workers:root["workers"].as<std::size_t>();std::vector<airside::autonomy::AutonomyRunRequest> requests;std::size_t ordinal=0;for(const auto& level:root["gnss_sigma_m"]){const auto sigma=level.as<double>();for(std::size_t i=0;i<count;++i){auto run=scenario;run.sensors.gnss_sigma_m=sigma;requests.push_back({std::move(run),first+static_cast<std::uint64_t>(i),ordinal++});}}const auto report=airside::autonomy::execute_runs(std::move(requests),workers);std::cout<<std::fixed<<std::setprecision(3)<<"RampLab autonomy experiment: "<<root["name"].as<std::string>()<<"\nRuns: "<<report.runs.size()<<"  Workers: "<<report.worker_count<<"  Wall time: "<<report.wall_time.count()<<" s  Runs/s: "<<static_cast<double>(report.runs.size())/report.wall_time.count()<<"\n";for(const auto& level:root["gnss_sigma_m"]){const auto sigma=level.as<double>();std::vector<double> times,errors,clearances;std::size_t successes=0,collisions=0;for(const auto&r:report.runs)if(r.gnss_sigma_m==sigma){successes+=r.metrics.result==airside::autonomy::MissionResult::Success;collisions+=r.metrics.collision_count;times.push_back(r.metrics.completion_time_s);errors.push_back(r.metrics.maximum_route_error_m);clearances.push_back(r.metrics.minimum_obstacle_clearance_m);}std::cout<<"GNSS sigma "<<sigma<<" m: "<<successes<<"/"<<times.size()<<" successes, mean completion "<<mean(times)<<" s, P95 max route error "<<p95(errors)<<" m, mean minimum clearance "<<mean(clearances)<<" m, collisions "<<collisions<<"\n";}return 0;}catch(const std::exception&e){std::cerr<<"autonomy experiment: "<<e.what()<<'\n';return 1;}}
