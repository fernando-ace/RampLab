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
    EXPECT_GT(result.near_conflict_events,0U);EXPECT_GT(result.forced_safety_stops,0U);EXPECT_EQ(result.deadlock_count,0U);
    EXPECT_EQ(result.retreat_count,0U);
    EXPECT_TRUE(std::ranges::any_of(result.events,[](const auto& event){return event.kind==TrafficEventKind::Waiting&&event.resource.starts_with("edge/");}));
    EXPECT_TRUE(std::ranges::any_of(result.events,[](const auto& event){return event.kind==TrafficEventKind::EnteredConflict&&event.resource.starts_with("edge/");}));
    EXPECT_TRUE(std::ranges::any_of(result.events,[](const auto& event){return event.kind==TrafficEventKind::ForcedSafetyStop&&event.resource.starts_with("edge/");}));
}
TEST(FleetScenarioTest, OpposingVehiclesReserveNarrowEdgeBeforeLeavingHoldingBays){
    const auto scenario=load_fleet_scenario(std::filesystem::path{AIRSIDE_SOURCE_DIR}/"scenarios/autonomy_fleet_opposing.yaml");
    FleetSimulation fleet{scenario.vehicle_scenario,scenario.missions,42};
    while(fleet.advance()){}
    const auto result=fleet.result();
    ASSERT_EQ(result.vehicle_count,3U);EXPECT_EQ(result.missions_completed,3U);EXPECT_EQ(result.safe_timeouts,0U);
    EXPECT_EQ(result.collisions,0U);EXPECT_GT(result.minimum_separation_m,2.0);
    EXPECT_GT(result.traffic_waiting_time_s,30.0);EXPECT_EQ(result.reservation_contentions,1U);
    EXPECT_EQ(result.retreat_count,0U);
    const auto deferred=std::ranges::find_if(result.events,[](const auto& event){
        return event.kind==TrafficEventKind::Deferred&&event.vehicle.value=="tug_02";
    });
    ASSERT_NE(deferred,result.events.end());EXPECT_DOUBLE_EQ(deferred->time_s,0.0);
    EXPECT_EQ(deferred->other.value,"tug_01");
    const auto entered=std::ranges::find_if(result.events,[](const auto& event){
        return event.kind==TrafficEventKind::EnteredConflict&&event.resource.starts_with("edge/");
    });
    ASSERT_NE(entered,result.events.end());EXPECT_EQ(entered->vehicle.value,"tug_01");
    const auto released=std::ranges::find_if(result.events,[](const auto& event){
        return event.kind==TrafficEventKind::ReleasedConflict&&event.vehicle.value=="tug_01";
    });
    ASSERT_NE(released,result.events.end());
    const auto granted=std::ranges::find_if(released,result.events.end(),[](const auto& event){
        return event.kind==TrafficEventKind::Granted&&event.vehicle.value=="tug_02";
    });
    ASSERT_NE(granted,result.events.end());EXPECT_GE(granted->time_s,released->time_s);
}
TEST(FleetScenarioTest, DynamicClosureReplansBeforeVehicleCanEnterClosedEdge){
    const auto scenario=load_fleet_scenario(std::filesystem::path{AIRSIDE_SOURCE_DIR}/"scenarios/autonomy_fleet_dynamic_closure.yaml");
    ASSERT_EQ(scenario.road_events.size(),2U);
    FleetSimulation fleet{scenario.vehicle_scenario,scenario.missions,42,scenario.road_events,scenario.deadlock_persistence_s,scenario.resource_specific_tie_breaks};
    while(fleet.advance()){}
    const auto result=fleet.result();
    EXPECT_EQ(result.missions_completed,2U);EXPECT_EQ(result.safe_timeouts,0U);EXPECT_EQ(result.collisions,0U);EXPECT_GT(result.total_distance_m,300.0);
    EXPECT_EQ(result.retreat_count,0U);
    EXPECT_EQ(result.road_closure_replans,1U);EXPECT_GE(result.reroutes,1U);
    EXPECT_TRUE(std::ranges::any_of(result.events,[](const auto& event){return event.kind==TrafficEventKind::RoadClosed;}));
    EXPECT_TRUE(std::ranges::any_of(result.events,[](const auto& event){return event.kind==TrafficEventKind::RoadReopened;}));
    EXPECT_TRUE(std::ranges::any_of(result.events,[](const auto& event){return event.kind==TrafficEventKind::Reroute&&event.vehicle.value=="tug_01";}));
    const auto unaffected=std::ranges::find(result.vehicles,VehicleId{"tug_02"},&FleetVehicleResult::id);
    ASSERT_NE(unaffected,result.vehicles.end());EXPECT_EQ(unaffected->metrics.result,MissionResult::Success);
}
TEST(FleetExperimentTest, DynamicRoadChangesAreIdenticalAcrossSerialAndParallelWorkers){
    const auto scenario=load_fleet_scenario(std::filesystem::path{AIRSIDE_SOURCE_DIR}/"scenarios/autonomy_fleet_dynamic_closure.yaml");
    std::vector<FleetRunRequest> requests;
    for(std::size_t i=0;i<8;++i)requests.push_back({scenario,42,i});
    const auto serial=execute_fleet_runs(requests,1),parallel=execute_fleet_runs(std::move(requests),4);
    ASSERT_EQ(serial.runs.size(),parallel.runs.size());
    for(std::size_t i=0;i<serial.runs.size();++i){
        EXPECT_EQ(serial.runs[i].metrics,parallel.runs[i].metrics);
        EXPECT_EQ(serial.runs[i].metrics.missions_completed,2U);
        EXPECT_EQ(serial.runs[i].metrics.collisions,0U);
        EXPECT_EQ(serial.runs[i].metrics.road_closure_replans,1U);
    }
}
TEST(FleetScenarioTest, EightVehicleOpposingCorridorsCompleteWithoutCollisions){
    const auto scenario=load_fleet_scenario(std::filesystem::path{AIRSIDE_SOURCE_DIR}/"scenarios/autonomy_fleet_congested.yaml");
    FleetSimulation fleet{scenario.vehicle_scenario,scenario.missions,42,scenario.road_events,scenario.deadlock_persistence_s,scenario.resource_specific_tie_breaks};
    while(fleet.advance()){}
    const auto result=fleet.result();
    EXPECT_EQ(result.vehicle_count,8U);EXPECT_EQ(result.missions_attempted,8U);EXPECT_EQ(result.missions_completed,8U);
    EXPECT_EQ(result.safe_timeouts,0U);EXPECT_EQ(result.collisions,0U);EXPECT_GT(result.minimum_separation_m,2.0);
    EXPECT_GT(result.reservation_contentions,0U);EXPECT_GT(result.forced_safety_stops,0U);
    EXPECT_GT(result.traffic_waiting_time_s,0.0);
    EXPECT_EQ(result.retreat_count,0U);
}
TEST(FleetScenarioTest, ThreeVehicleWaitForCycleTriggersPersistentRecovery){
    const auto scenario=load_fleet_scenario(std::filesystem::path{AIRSIDE_SOURCE_DIR}/"scenarios/autonomy_fleet_deadlock.yaml");
    FleetSimulation fleet{scenario.vehicle_scenario,scenario.missions,42,scenario.road_events,scenario.deadlock_persistence_s,scenario.resource_specific_tie_breaks};
    bool observed_cycle=false;
    while(fleet.advance()) if(fleet.deadlocked_vehicles().size()>=3) observed_cycle=true;
    const auto result=fleet.result();
    EXPECT_TRUE(observed_cycle);EXPECT_EQ(result.deadlock_count,1U);EXPECT_EQ(result.deadlocks_resolved,1U);
    EXPECT_EQ(result.missions_completed,3U);EXPECT_EQ(result.safe_timeouts,0U);
    EXPECT_EQ(result.recovery_attempts,1U);EXPECT_EQ(result.retreat_count,1U);EXPECT_GE(result.reroutes,1U);
    EXPECT_EQ(result.collisions,0U);EXPECT_GT(result.minimum_separation_m,2.0);
    EXPECT_TRUE(result.wait_dependencies.empty());EXPECT_TRUE(result.deadlocked_vehicles.empty());
    EXPECT_EQ(result.outstanding_reservations,0U);
    const auto detected=std::ranges::find_if(result.events,[](const auto& event){return event.kind==TrafficEventKind::DeadlockDetected;});
    const auto recovery=std::ranges::find_if(result.events,[](const auto& event){return event.kind==TrafficEventKind::DeadlockRecovery;});
    const auto selected=std::ranges::find_if(result.events,[](const auto& event){return event.kind==TrafficEventKind::RetreatSelected;});
    const auto started=std::ranges::find_if(result.events,[](const auto& event){return event.kind==TrafficEventKind::RetreatStarted;});
    const auto completed=std::ranges::find_if(result.events,[](const auto& event){return event.kind==TrafficEventKind::RetreatCompleted;});
    const auto released=std::ranges::find_if(result.events,[](const auto& event){return event.kind==TrafficEventKind::RetreatResourceReleased;});
    const auto resumed=std::ranges::find_if(result.events,[](const auto& event){return event.kind==TrafficEventKind::MissionResumed;});
    const auto resolved=std::ranges::find_if(result.events,[](const auto& event){return event.kind==TrafficEventKind::RecoveryResolved;});
    ASSERT_NE(detected,result.events.end());ASSERT_NE(recovery,result.events.end());ASSERT_NE(selected,result.events.end());
    ASSERT_NE(started,result.events.end());ASSERT_NE(completed,result.events.end());ASSERT_NE(released,result.events.end());
    ASSERT_NE(resumed,result.events.end());ASSERT_NE(resolved,result.events.end());
    EXPECT_EQ(selected->vehicle.value,"tug_01");
    EXPECT_EQ(selected->resource,"intersection/2");
    const auto opposing_wait=std::ranges::find_if(result.events,[&](const auto& event){
        return event.kind==TrafficEventKind::Deferred&&event.vehicle==selected->vehicle&&event.resource==selected->resource;
    });
    ASSERT_NE(opposing_wait,result.events.end());EXPECT_EQ(opposing_wait->other.value,"tug_02");
    const auto contested_owner_clear=std::ranges::find_if(result.events,[&](const auto& event){
        return event.kind==TrafficEventKind::ReleasedConflict&&event.vehicle==opposing_wait->other&&event.resource==selected->resource;
    });
    ASSERT_NE(contested_owner_clear,result.events.end());
    EXPECT_LT(std::distance(result.events.begin(),contested_owner_clear),std::distance(result.events.begin(),released));
    const auto yielding_mission=std::ranges::find(result.vehicles,VehicleId{"tug_01"},&FleetVehicleResult::id);
    const auto opposing_mission=std::ranges::find(result.vehicles,VehicleId{"tug_02"},&FleetVehicleResult::id);
    ASSERT_NE(yielding_mission,result.vehicles.end());ASSERT_NE(opposing_mission,result.vehicles.end());
    EXPECT_EQ(yielding_mission->metrics.result,MissionResult::Success);
    EXPECT_EQ(opposing_mission->metrics.result,MissionResult::Success);
    EXPECT_LT(std::distance(result.events.begin(),detected),std::distance(result.events.begin(),recovery));
    EXPECT_LT(std::distance(result.events.begin(),recovery),std::distance(result.events.begin(),selected));
    EXPECT_LT(std::distance(result.events.begin(),selected),std::distance(result.events.begin(),started));
    EXPECT_LT(std::distance(result.events.begin(),started),std::distance(result.events.begin(),completed));
    EXPECT_LT(std::distance(result.events.begin(),completed),std::distance(result.events.begin(),released));
    EXPECT_LT(std::distance(result.events.begin(),released),std::distance(result.events.begin(),resumed));
    EXPECT_LT(std::distance(result.events.begin(),resumed),std::distance(result.events.begin(),resolved));
    const double reverse_x=completed->position.x_m-started->position.x_m;
    const double reverse_y=completed->position.y_m-started->position.y_m;
    const double forward_x=started->position.x_m-started->target.x_m;
    const double forward_y=started->position.y_m-started->target.y_m;
    EXPECT_GT(std::hypot(reverse_x,reverse_y),5.0);
    EXPECT_LT(reverse_x*forward_x+reverse_y*forward_y,0.0);
}
TEST(FleetScenarioTest, UnrecoverableCycleFailsSafelyWithoutRetryLoop){
    auto scenario=load_fleet_scenario(std::filesystem::path{AIRSIDE_SOURCE_DIR}/"scenarios/autonomy_fleet_deadlock.yaml");
    scenario.deadlock_persistence_s=0.1;
    scenario.vehicle_scenario.limits.maximum_speed_mps=0.1;
    scenario.vehicle_scenario.limits.maximum_acceleration_mps2=0.01;
    scenario.missions[0].start_node="Cycle A";
    scenario.missions[1].start_node="Cycle B";
    scenario.missions[2].start_node="Cycle C";
    FleetSimulation fleet{scenario.vehicle_scenario,scenario.missions,42,scenario.road_events,
                          scenario.deadlock_persistence_s,scenario.resource_specific_tie_breaks};
    std::size_t steps=0;
    while(fleet.advance()&&++steps<15001){}
    const auto result=fleet.result();
    EXPECT_LT(steps,15001U);EXPECT_EQ(result.deadlock_count,1U);EXPECT_EQ(result.recovery_attempts,1U);
    EXPECT_EQ(result.retreat_count,0U);EXPECT_EQ(result.safe_timeouts,3U);EXPECT_EQ(result.collisions,0U);
    EXPECT_EQ(std::ranges::count_if(result.events,[](const auto& event){return event.kind==TrafficEventKind::RetreatFailed;}),1);
    EXPECT_FALSE(std::ranges::any_of(result.events,[](const auto& event){return event.kind==TrafficEventKind::RetreatStarted;}));
}
TEST(FleetExperimentTest, ThreeVehicleDeadlockRecoveryIsIdenticalAcrossSerialAndParallelWorkers){
    const auto scenario=load_fleet_scenario(std::filesystem::path{AIRSIDE_SOURCE_DIR}/"scenarios/autonomy_fleet_deadlock.yaml");
    std::vector<FleetRunRequest> requests;for(std::size_t i=0;i<4;++i)requests.push_back({scenario,42,i});
    const auto serial=execute_fleet_runs(requests,1),parallel=execute_fleet_runs(std::move(requests),4);
    ASSERT_EQ(serial.runs.size(),parallel.runs.size());
    for(std::size_t i=0;i<serial.runs.size();++i){
        EXPECT_EQ(serial.runs[i].metrics,parallel.runs[i].metrics);
        EXPECT_EQ(serial.runs[i].metrics.deadlock_count,1U);EXPECT_EQ(serial.runs[i].metrics.deadlocks_resolved,1U);
        EXPECT_EQ(serial.runs[i].metrics.recovery_attempts,1U);EXPECT_EQ(serial.runs[i].metrics.retreat_count,1U);
        EXPECT_EQ(serial.runs[i].metrics.missions_completed,3U);EXPECT_EQ(serial.runs[i].metrics.collisions,0U);
    }
}
TEST(FleetExperimentTest, EightVehicleCongestionIsIdenticalAcrossSerialAndParallelWorkers){
    const auto scenario=load_fleet_scenario(std::filesystem::path{AIRSIDE_SOURCE_DIR}/"scenarios/autonomy_fleet_congested.yaml");
    std::vector<FleetRunRequest> requests;for(std::size_t i=0;i<8;++i)requests.push_back({scenario,42,i});
    const auto serial=execute_fleet_runs(requests,1),parallel=execute_fleet_runs(std::move(requests),4);
    ASSERT_EQ(serial.runs.size(),parallel.runs.size());
    for(std::size_t i=0;i<serial.runs.size();++i){
        EXPECT_EQ(serial.runs[i].ordinal,parallel.runs[i].ordinal);
        EXPECT_EQ(serial.runs[i].metrics,parallel.runs[i].metrics);
        EXPECT_EQ(serial.runs[i].metrics.missions_completed,8U);
        EXPECT_EQ(serial.runs[i].metrics.collisions,0U);
    }
}
TEST(FleetTest, WaitForDependenciesExposeBlockerResourceAndSimulationDuration){
    FleetSimulation fleet{base_scenario(),competing_missions(),42};
    bool observed=false;
    while(fleet.advance()){
        const auto dependencies=fleet.wait_dependencies();
        if(!dependencies.empty()&&dependencies.front().wait_duration_s>0.05){
            observed=true;EXPECT_FALSE(dependencies.front().waiting_vehicle.value.empty());
            EXPECT_FALSE(dependencies.front().blocking_vehicle.value.empty());
            EXPECT_FALSE(dependencies.front().resource.empty());EXPECT_GT(dependencies.front().wait_duration_s,0.05);
            break;
        }
    }
    EXPECT_TRUE(observed);
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
TEST(FleetReservationTest, CancelledStaleWaiterDoesNotReceiveReleasedResource){
    TrafficReservationTable table;
    ASSERT_TRUE(table.request("edge_shared",{VehicleId{"tug_a"},1.0,0}));
    EXPECT_FALSE(table.request("edge_shared",{VehicleId{"tug_b"},2.0,0}));
    table.retain_waiters("edge_shared",{});
    ASSERT_TRUE(table.release("edge_shared",VehicleId{"tug_a"}));
    EXPECT_FALSE(table.owner("edge_shared"));
}
TEST(FleetReservationTest, PruningStaleWaiterPreservesActiveWaiterOrdering){
    TrafficReservationTable table;
    ASSERT_TRUE(table.request("edge_shared",{VehicleId{"tug_a"},1.0,0}));
    EXPECT_FALSE(table.request("edge_shared",{VehicleId{"tug_b"},2.0,0}));
    EXPECT_FALSE(table.request("edge_shared",{VehicleId{"tug_c"},3.0,0}));
    table.retain_waiters("edge_shared",{VehicleId{"tug_c"}});
    ASSERT_TRUE(table.release("edge_shared",VehicleId{"tug_a"}));
    ASSERT_TRUE(table.owner("edge_shared"));
    EXPECT_EQ(table.owner("edge_shared")->value,"tug_c");
}
TEST(FleetReservationTest, OlderWaiterCannotBeStarvedByLaterHighPriorityRequest){
    TrafficReservationTable table;
    ASSERT_TRUE(table.request("edge_fair",{VehicleId{"tug_owner"},0.0,0}));
    EXPECT_FALSE(table.request("edge_fair",{VehicleId{"tug_older"},1.0,8}));
    EXPECT_FALSE(table.request("edge_fair",{VehicleId{"tug_priority"},2.0,-10}));
    ASSERT_TRUE(table.release("edge_fair",VehicleId{"tug_owner"}));
    ASSERT_TRUE(table.owner("edge_fair"));EXPECT_EQ(table.owner("edge_fair")->value,"tug_older");
    EXPECT_EQ(table.starvation_preventions(),1U);
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
TEST(FleetDeadlockTest, SelectsYieldVehicleByPriorityWaitThenStableId){
    const auto selected=choose_recovery_vehicle({
        {VehicleId{"tug_c"},3,100.0},{VehicleId{"tug_b"},5,4.0},
        {VehicleId{"tug_a"},5,4.0},{VehicleId{"tug_d"},5,3.0}});
    EXPECT_EQ(selected.value,"tug_a");
    EXPECT_THROW((void)choose_recovery_vehicle({}),std::invalid_argument);
}
TEST(FleetTest, ScenarioLoadsThreeMissionsAndConfiguredFault){
    const auto path=std::filesystem::path{AIRSIDE_SOURCE_DIR}/"scenarios/autonomy_fleet_fault.yaml";
    const auto scenario=load_fleet_scenario(path);ASSERT_EQ(scenario.missions.size(),3U);
    EXPECT_EQ(scenario.missions[1].id.value,"tug_02");ASSERT_EQ(scenario.missions[1].faults.size(),1U);
    EXPECT_EQ(scenario.missions[1].faults.front().sensor,SensorKind::Gnss);
}
TEST(FleetDispatcherScenarioTest, LoadsVehicleCapabilitiesAndDynamicallyReleasedServiceRequests){
    const auto scenario=load_fleet_scenario(std::filesystem::path{AIRSIDE_SOURCE_DIR}/"scenarios/autonomy_dispatch_dynamic.yaml");
    EXPECT_TRUE(scenario.missions.empty());
    ASSERT_EQ(scenario.dispatch_fleet.size(),3U);
    ASSERT_EQ(scenario.service_requests.size(),4U);
    EXPECT_EQ(scenario.dispatch_fleet.front().id.value,"baggage_01");
    EXPECT_EQ(scenario.service_requests[0].id.value,"turn_a1");
    EXPECT_EQ(scenario.service_requests[1].release_time_s,400.0);
    EXPECT_EQ(scenario.service_requests[1].kind,ServiceKind::FuelService);
    EXPECT_DOUBLE_EQ(scenario.dispatch_aging_interval_s,60.0);
}
TEST(FleetExperimentTest, SerialAndParallelFleetResultsAreIdentical){
    const auto scenario=load_fleet_scenario(std::filesystem::path{AIRSIDE_SOURCE_DIR}/"scenarios/autonomy_fleet_fault.yaml");
    std::vector<FleetRunRequest> requests;for(std::size_t i=0;i<24;++i)requests.push_back({scenario,42+i,100-i});
    const auto serial=execute_fleet_runs(requests,1),parallel=execute_fleet_runs(std::move(requests),4);
    ASSERT_EQ(serial.runs.size(),parallel.runs.size());
    for(std::size_t i=0;i<serial.runs.size();++i){
        const auto& a=serial.runs[i];const auto& b=parallel.runs[i];
        EXPECT_EQ(a.ordinal,b.ordinal);EXPECT_EQ(a.metrics,b.metrics);
        EXPECT_EQ(a.metrics.missions_completed,3U);EXPECT_EQ(a.metrics.collisions,0U);
    }
}
TEST(FleetExperimentTest, OpposingEdgeFleetResultsMatchSerialAndParallelExactly){
    const auto scenario=load_fleet_scenario(std::filesystem::path{AIRSIDE_SOURCE_DIR}/"scenarios/autonomy_fleet_opposing.yaml");
    std::vector<FleetRunRequest> requests;
    for(std::size_t i=0;i<10;++i)requests.push_back({scenario,42+i,i});
    const auto serial=execute_fleet_runs(requests,1),parallel=execute_fleet_runs(std::move(requests),4);
    ASSERT_EQ(serial.runs.size(),parallel.runs.size());
    for(std::size_t i=0;i<serial.runs.size();++i){
        EXPECT_EQ(serial.runs[i].ordinal,parallel.runs[i].ordinal);
        EXPECT_EQ(serial.runs[i].metrics,parallel.runs[i].metrics);
        EXPECT_EQ(serial.runs[i].metrics.missions_completed,3U);
        EXPECT_EQ(serial.runs[i].metrics.collisions,0U);
    }
}
TEST(FleetDispatcherIntegrationTest, DynamicScenarioCompletesWithoutCollisionOrLeakedReservations){
    const auto scenario=load_fleet_scenario(std::filesystem::path{AIRSIDE_SOURCE_DIR}/"scenarios/autonomy_dispatch_dynamic.yaml");
    FleetSimulation simulation{scenario,42};
    while(simulation.advance()){}
    const auto result=simulation.result();
    EXPECT_EQ(result.dispatch.requests_created,4U);
    EXPECT_EQ(result.dispatch.requests_completed,4U);
    EXPECT_EQ(result.dispatch.requests_failed,0U);
    EXPECT_EQ(result.dispatch.assignments,4U);
    EXPECT_EQ(result.dispatch.unfinished_requests,0U);
    EXPECT_EQ(result.collisions,0U);
    EXPECT_EQ(result.outstanding_reservations,0U);
    EXPECT_EQ(result.deadlocked_vehicles.size(),0U);
}
TEST(FleetDispatcherIntegrationTest, AgingScenarioServesHighPriorityFirstAndPromotesOldWork){
    const auto scenario=load_fleet_scenario(std::filesystem::path{AIRSIDE_SOURCE_DIR}/"scenarios/autonomy_dispatch_fairness.yaml");
    FleetSimulation simulation{scenario,42};while(simulation.advance()){}const auto result=simulation.result();
    ASSERT_EQ(result.dispatch.requests_completed,7U);EXPECT_EQ(result.dispatch.requests_failed,0U);
    EXPECT_GT(result.dispatch.aging_activations,0U);EXPECT_EQ(result.collisions,0U);EXPECT_EQ(result.outstanding_reservations,0U);
    const auto high=std::ranges::find_if(result.dispatch.events,[](const auto& e){return e.kind==DispatchEventKind::Assigned;});
    ASSERT_NE(high,result.dispatch.events.end());EXPECT_EQ(high->request.value,"high_00");
    const auto low=std::ranges::find_if(result.dispatch.events,[](const auto& e){return e.request.value=="low_waiting"&&e.kind==DispatchEventKind::Assigned;});
    const auto last_high=std::ranges::find_if(result.dispatch.events,[](const auto& e){return e.request.value=="high_10"&&e.kind==DispatchEventKind::Assigned;});
    ASSERT_NE(low,result.dispatch.events.end());ASSERT_NE(last_high,result.dispatch.events.end());EXPECT_LT(low->time_s,last_high->time_s);
}
TEST(FleetDispatcherIntegrationTest, UnavailableVehicleReassignsTaskAndBackupCompletesSafely){
    const auto scenario=load_fleet_scenario(std::filesystem::path{AIRSIDE_SOURCE_DIR}/"scenarios/autonomy_dispatch_reassignment.yaml");
    FleetSimulation simulation{scenario,42};while(simulation.advance()){}const auto result=simulation.result();
    ASSERT_EQ(result.dispatch.requests.size(),1U);const auto& task=result.dispatch.requests.front();
    EXPECT_EQ(task.state,ServiceTaskState::Completed);ASSERT_TRUE(task.assigned_vehicle);EXPECT_EQ(task.assigned_vehicle->value,"baggage_backup");
    EXPECT_EQ(task.reassignments,1U);EXPECT_EQ(result.dispatch.reassignments,1U);EXPECT_EQ(result.dispatch.requests_failed,0U);
    EXPECT_EQ(result.collisions,0U);EXPECT_EQ(result.outstanding_reservations,0U);EXPECT_TRUE(result.deadlocked_vehicles.empty());
    EXPECT_TRUE(std::ranges::any_of(result.dispatch.events,[](const auto& e){return e.kind==DispatchEventKind::Reassigned&&e.vehicle.value=="baggage_backup";}));
}
TEST(FleetDispatcherIntegrationTest, DependencyUnlockedRequestUsesLiveReservationsAndCollisionChecks){
    auto scenario=load_fleet_scenario(std::filesystem::path{AIRSIDE_SOURCE_DIR}/"scenarios/autonomy_dispatch_dynamic.yaml");
    scenario.service_requests.clear();
    FleetSimulation simulation{scenario,42};
    EXPECT_TRUE(simulation.finished());
    ServiceRequest request{ServiceRequestId{"turnaround_mobile"},ServiceKind::BaggageDelivery,"baggage_delivery",
        "Service Depot","Gate A1",0.0,20,1500.0,30.0};
    simulation.add_service_request(request);
    const auto original=simulation.service_requests();
    ASSERT_EQ(original.size(),1U);
    std::size_t steps=0;
    while(simulation.advance()&&++steps<15000){}
    const auto result=simulation.result();
    ASSERT_EQ(result.dispatch.requests.size(),1U);
    EXPECT_EQ(result.dispatch.requests.front().state,ServiceTaskState::Completed);
    EXPECT_EQ(result.dispatch.requests_completed,1U);
    EXPECT_EQ(result.dispatch.requests_failed,0U);
    EXPECT_EQ(result.collisions,0U);
    EXPECT_EQ(result.outstanding_reservations,0U);
    EXPECT_FALSE(result.dispatch.events.empty());
}
TEST(FleetExperimentTest, DynamicDispatchMatchesSerialParallelAndIndependentRuns){
    const auto scenario=load_fleet_scenario(std::filesystem::path{AIRSIDE_SOURCE_DIR}/"scenarios/autonomy_dispatch_dynamic.yaml");
    std::vector<FleetRunRequest> requests;for(std::size_t i=0;i<4;++i)requests.push_back({scenario,42,i});
    const auto serial=execute_fleet_runs(requests,1),parallel=execute_fleet_runs(std::move(requests),4);
    ASSERT_EQ(serial.runs.size(),parallel.runs.size());
    for(std::size_t i=0;i<serial.runs.size();++i){
        EXPECT_EQ(serial.runs[i].ordinal,parallel.runs[i].ordinal);
        EXPECT_EQ(serial.runs[i].metrics,parallel.runs[i].metrics);
        EXPECT_EQ(serial.runs[i].metrics.deterministic_digest,parallel.runs[i].metrics.deterministic_digest);
        EXPECT_EQ(serial.runs[i].metrics.dispatch.requests_completed,4U);
        EXPECT_EQ(serial.runs[i].metrics.collisions,0U);
    }
    EXPECT_EQ(serial.runs.front().metrics,serial.runs.back().metrics);
}
}
