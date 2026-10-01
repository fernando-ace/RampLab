#include "airside/autonomy/fleet.hpp"
#include "airside/autonomy/scenario_loader.hpp"
#include "airside/autonomy/experiment.hpp"

#include <gtest/gtest.h>
#include <algorithm>
#include <filesystem>

namespace airside::autonomy {
namespace {
AutonomyScenario base_scenario(){return load_scenario(std::filesystem::path{AIRSIDE_SOURCE_DIR}/"scenarios/autonomy_tug.yaml");}
std::vector<FleetMission> competing_missions(){
    return {{VehicleId{"tug_01"},"Service Depot","Gate A2",0,{}},
            {VehicleId{"tug_02"},"South Junction","Gate A1",0,{}},
            {VehicleId{"tug_03"},"Gate A3","Service Depot",0,{}}};
}
FleetMetrics run_fleet(){FleetSimulation fleet{base_scenario(),competing_missions(),42};std::size_t steps=0;while(fleet.advance()&&++steps<15000){}return fleet.result();}
}

TEST(FleetTest, ThreeVehiclesShareClockAndResolveCompetingTrafficSafely){
    const auto result=run_fleet();
    ASSERT_EQ(result.vehicle_count,3U);EXPECT_EQ(result.missions_attempted,3U);
    EXPECT_GT(result.reservation_requests,0U);EXPECT_GT(result.reservation_contentions,0U);
    EXPECT_GT(result.traffic_waiting_time_s,0.0);EXPECT_EQ(result.collisions,0U);
    EXPECT_EQ(result.vehicles.size(),3U);EXPECT_GT(result.minimum_separation_m,2.0);
    EXPECT_TRUE(std::ranges::any_of(result.events,[](const auto& event){return event.kind==TrafficEventKind::Request&&(event.resource.starts_with("edge/")||event.resource.starts_with("intersection/"));}));
}

TEST(FleetTest, SameSeedProducesIdenticalFleetEventAndMetricDigest){
    const auto a=run_fleet(),b=run_fleet();
    EXPECT_EQ(a.deterministic_digest,b.deterministic_digest);
    EXPECT_EQ(a.missions_completed,b.missions_completed);EXPECT_EQ(a.collisions,b.collisions);
    EXPECT_DOUBLE_EQ(a.traffic_waiting_time_s,b.traffic_waiting_time_s);
    ASSERT_EQ(a.events.size(),b.events.size());
    for(std::size_t i=0;i<a.events.size();++i){EXPECT_DOUBLE_EQ(a.events[i].time_s,b.events[i].time_s);EXPECT_EQ(a.events[i].kind,b.events[i].kind);EXPECT_EQ(a.events[i].vehicle,b.events[i].vehicle);}
}
TEST(FleetTest, AggregateMetricsMatchVehicleResultsAndCommonCompletionHorizon){
    const auto result=run_fleet();
    ASSERT_EQ(result.missions_completed,3U);
    double total_distance=0.0,completion_horizon=0.0;
    for(const auto& vehicle:result.vehicles){total_distance+=vehicle.metrics.distance_traveled_m;completion_horizon=std::max(completion_horizon,vehicle.metrics.completion_time_s);}
    EXPECT_NEAR(result.total_distance_m,total_distance,1e-9);
    EXPECT_DOUBLE_EQ(result.total_mission_time_s,completion_horizon);
    EXPECT_DOUBLE_EQ(result.cumulative_waiting_time_s,result.traffic_waiting_time_s);
    EXPECT_DOUBLE_EQ(result.throughput_per_simulated_hour,3.0*3600.0/completion_horizon);
    EXPECT_EQ(result.collisions,0U);
}
TEST(FleetScenarioTest, IndependentTrafficCompletesWithoutReservations){
    const auto scenario=load_fleet_scenario(std::filesystem::path{AIRSIDE_SOURCE_DIR}/"scenarios/autonomy_fleet_independent.yaml");
    FleetSimulation fleet{scenario.vehicle_scenario,scenario.missions,42};while(fleet.advance()){}const auto result=fleet.result();
    EXPECT_EQ(result.vehicle_count,3U);EXPECT_EQ(result.missions_completed,3U);EXPECT_EQ(result.safe_timeouts,0U);
    EXPECT_EQ(result.reservation_contentions,0U);EXPECT_DOUBLE_EQ(result.traffic_waiting_time_s,0.0);EXPECT_EQ(result.collisions,0U);
}
TEST(FleetScenarioTest, SharedSegmentMergeWaitsThenCompletesWithoutCollision){
    const auto scenario=load_fleet_scenario(std::filesystem::path{AIRSIDE_SOURCE_DIR}/"scenarios/autonomy_fleet_narrow_segment.yaml");
    FleetSimulation fleet{scenario.vehicle_scenario,scenario.missions,42};while(fleet.advance()){}const auto result=fleet.result();
    EXPECT_EQ(result.vehicle_count,3U);EXPECT_EQ(result.missions_completed,3U);EXPECT_EQ(result.safe_timeouts,0U);
    EXPECT_GT(result.reservation_contentions,0U);EXPECT_GT(result.traffic_waiting_time_s,0.0);EXPECT_EQ(result.collisions,0U);
    EXPECT_GT(result.near_conflict_events,0U);EXPECT_GT(result.forced_safety_stops,0U);
    EXPECT_TRUE(std::ranges::any_of(result.events,[](const auto& event){return event.kind==TrafficEventKind::Waiting&&event.resource.starts_with("edge/");}));
    EXPECT_TRUE(std::ranges::any_of(result.events,[](const auto& event){return event.kind==TrafficEventKind::EnteredConflict&&event.resource.starts_with("edge/");}));
    EXPECT_TRUE(std::ranges::any_of(result.events,[](const auto& event){return event.kind==TrafficEventKind::ForcedSafetyStop&&event.resource.starts_with("edge/");}));
}
TEST(FleetTest, DifferentSeedsChangeStochasticVehicleTrajectories){
    auto a=competing_missions();auto b=competing_missions();
    FleetSimulation first{base_scenario(),std::move(a),42},second{base_scenario(),std::move(b),43};
    while(first.advance()){}while(second.advance()){}const auto x=first.result(),y=second.result();
    ASSERT_EQ(x.vehicles.size(),y.vehicles.size());
    EXPECT_NE(x.vehicles[0].metrics.trajectory_digest,y.vehicles[0].metrics.trajectory_digest);
}
TEST(FleetTest, IndependentSafetyValidatorDetectsOverlappingVehicles){
    std::vector<FleetMission> missions{{VehicleId{"same_01"},"Service Depot","Gate A2",0,{}},
                                       {VehicleId{"same_02"},"Service Depot","Gate A1",0,{}}};
    FleetSimulation fleet{base_scenario(),std::move(missions),42};
    (void)fleet.advance();const auto result=fleet.result();
    EXPECT_GT(result.collisions,0U);EXPECT_LT(result.minimum_separation_m,2.0);
}

TEST(FleetTest, FaultedMemberRetainsPerVehicleFaultMetrics){
    auto missions=competing_missions();
    missions[1].faults.push_back({SensorKind::Gnss,SensorFaultKind::Dropout,2.0,12.0});
    FleetSimulation fleet{base_scenario(),std::move(missions),42};
    std::size_t steps=0;while(fleet.advance()&&++steps<15000){}
    const auto result=fleet.result();ASSERT_EQ(result.vehicles.size(),3U);
    EXPECT_GT(result.vehicles[1].metrics.degraded_mode_entries,0U);
    EXPECT_EQ(result.collisions,0U);
}

TEST(FleetTest, VehicleIdentifiersMustBeUnique){
    auto missions=competing_missions();missions[1].id=missions[0].id;
    EXPECT_THROW((FleetSimulation{base_scenario(),std::move(missions),42}),std::invalid_argument);
}
TEST(FleetReservationTest, SameTimestampUsesPriorityThenVehicleId){
    TrafficReservationTable table;
    const auto winner=table.request_batch("edge_north_south",{
        {VehicleId{"tug_c"},4.0,0},{VehicleId{"tug_b"},4.0,-1},{VehicleId{"tug_a"},4.0,-1}});
    ASSERT_TRUE(winner);EXPECT_EQ(winner->value,"tug_a");
}
TEST(FleetReservationTest, RequestsQueueAndReleaseTransfersExclusiveOwnership){
    TrafficReservationTable table;
    ASSERT_TRUE(table.request("intersection_a2",{VehicleId{"tug_a"},1.0,0}));
    EXPECT_FALSE(table.request("intersection_a2",{VehicleId{"tug_b"},2.0,0}));
    ASSERT_TRUE(table.owner("intersection_a2"));EXPECT_EQ(table.owner("intersection_a2")->value,"tug_a");
    EXPECT_FALSE(table.release("intersection_a2",VehicleId{"tug_b"}));
    EXPECT_TRUE(table.release("intersection_a2",VehicleId{"tug_a"}));
    ASSERT_TRUE(table.owner("intersection_a2"));EXPECT_EQ(table.owner("intersection_a2")->value,"tug_b");
}
TEST(FleetDeadlockTest, DetectsEveryVehicleInCycleButNotVehiclesMerelyBlockedByIt){
    const auto cycle=find_deadlocked_vehicles({
        {VehicleId{"tug_a"},{VehicleId{"tug_b"}}},
        {VehicleId{"tug_b"},{VehicleId{"tug_c"}}},
        {VehicleId{"tug_c"},{VehicleId{"tug_a"}}},
        {VehicleId{"tug_d"},{VehicleId{"tug_a"}}}});
    ASSERT_EQ(cycle.size(),3U);
    EXPECT_EQ(cycle[0].value,"tug_a");EXPECT_EQ(cycle[1].value,"tug_b");EXPECT_EQ(cycle[2].value,"tug_c");
    EXPECT_TRUE(find_deadlocked_vehicles({
        {VehicleId{"tug_a"},{VehicleId{"tug_b"}}},
        {VehicleId{"tug_b"},{VehicleId{"tug_c"}}},
        {VehicleId{"tug_c"},{}}}).empty());
}
TEST(FleetTest, ScenarioLoadsThreeMissionsAndConfiguredFault){
    const auto path=std::filesystem::path{AIRSIDE_SOURCE_DIR}/"scenarios/autonomy_fleet_fault.yaml";
    const auto scenario=load_fleet_scenario(path);ASSERT_EQ(scenario.missions.size(),3U);
    EXPECT_EQ(scenario.missions[1].id.value,"tug_02");ASSERT_EQ(scenario.missions[1].faults.size(),1U);
    EXPECT_EQ(scenario.missions[1].faults.front().sensor,SensorKind::Gnss);
}
TEST(FleetExperimentTest, SerialAndParallelFleetResultsAreIdentical){
    const auto scenario=load_fleet_scenario(std::filesystem::path{AIRSIDE_SOURCE_DIR}/"scenarios/autonomy_fleet_fault.yaml");
    std::vector<FleetRunRequest> requests;for(std::size_t i=0;i<24;++i)requests.push_back({scenario,42+i,100-i});
    const auto serial=execute_fleet_runs(requests,1),parallel=execute_fleet_runs(std::move(requests),4);
    ASSERT_EQ(serial.runs.size(),parallel.runs.size());
    for(std::size_t i=0;i<serial.runs.size();++i){
        const auto& a=serial.runs[i];const auto& b=parallel.runs[i];
        EXPECT_EQ(a.ordinal,b.ordinal);EXPECT_EQ(a.metrics.deterministic_digest,b.metrics.deterministic_digest);
        EXPECT_EQ(a.metrics.collisions,b.metrics.collisions);EXPECT_DOUBLE_EQ(a.metrics.traffic_waiting_time_s,b.metrics.traffic_waiting_time_s);
        EXPECT_DOUBLE_EQ(a.metrics.total_distance_m,b.metrics.total_distance_m);
        EXPECT_EQ(a.metrics.missions_completed,3U);EXPECT_EQ(a.metrics.collisions,0U);
    }
}
}
