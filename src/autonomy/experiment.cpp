#include "airside/autonomy/experiment.hpp"
#include <algorithm>
#include <atomic>
#include <stdexcept>
#include <thread>
namespace airside::autonomy {
AutonomyExperimentReport execute_runs(std::vector<AutonomyRunRequest> requests,std::size_t workers){
    if(requests.empty())throw std::invalid_argument("autonomy experiment requires at least one run");
    if(workers==0)throw std::invalid_argument("autonomy experiment worker count must be positive");
    workers=std::min(workers,requests.size());
    std::vector<AutonomyExperimentRun> runs(requests.size());
    std::atomic_size_t cursor{0};
    const auto begin=std::chrono::steady_clock::now();
    std::vector<std::jthread> pool;pool.reserve(workers);
    for(std::size_t w=0;w<workers;++w)pool.emplace_back([&]{for(;;){const auto i=cursor.fetch_add(1,std::memory_order_relaxed);if(i>=requests.size())break;auto&request=requests[i];const auto sigma=request.scenario.sensors.gnss_sigma_m;const auto stop_range=request.scenario.safety_stop_range_m;AutonomySimulation simulation{std::move(request.scenario),request.seed};ReferenceController controller{stop_range};const auto result=simulation.run(controller);runs[i]={request.seed,request.ordinal,sigma,result.metrics};}});
    pool.clear();
    const auto elapsed=std::chrono::steady_clock::now()-begin;
    std::ranges::sort(runs,{},&AutonomyExperimentRun::ordinal);
    return{std::move(runs),workers,elapsed};
}
}
