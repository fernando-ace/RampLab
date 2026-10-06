#include "RampLabSimulationSubsystem.h"

#include "RampLabIntegration.h"
#include "SRampLabControlPanel.h"
#include "airside/core/event_stream.hpp"
#include "airside/scenario/scenario_loader.hpp"
#include "airside/autonomy/scenario_loader.hpp"

#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformMisc.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Layout/SBox.h"

#include <algorithm>
#include <filesystem>

namespace {

FString AircraftStateText(airside::AircraftState State)
{
    switch (State) {
    case airside::AircraftState::Scheduled: return TEXT("Scheduled / not arrived");
    case airside::AircraftState::Arriving: return TEXT("Arriving");
    case airside::AircraftState::AtGate: return TEXT("At gate");
    case airside::AircraftState::WaitingForServices: return TEXT("Waiting for services");
    case airside::AircraftState::ReadyForPushback: return TEXT("Ready");
    case airside::AircraftState::Departed: return TEXT("Departed");
    }
    return TEXT("Unknown");
}

FString VehicleStateText(airside::VehicleState State)
{
    switch (State) {
    case airside::VehicleState::Idle: return TEXT("Idle at depot");
    case airside::VehicleState::Assigned: return TEXT("Assigned");
    case airside::VehicleState::TravelingToAircraft: return TEXT("Traveling to aircraft");
    case airside::VehicleState::Servicing: return TEXT("Servicing aircraft");
    case airside::VehicleState::ReturningToDepot: return TEXT("Returning to depot");
    }
    return TEXT("Unknown");
}

FString FormatResult(const FString& Label, const airside::SimulationResult& Result)
{
    return FString::Printf(
        TEXT("%s\nAvg turnaround  %.1f min\nDelayed  %llu / %llu\nFuel utilization  %.1f%%\nBaggage utilization  %.1f%%"),
        *Label,
        Result.metrics.average_turnaround_seconds / 60.0,
        static_cast<unsigned long long>(Result.metrics.delayed_aircraft),
        static_cast<unsigned long long>(Result.metrics.aircraft.size()),
        Result.metrics.fuel_utilization * 100.0,
        Result.metrics.baggage_utilization * 100.0);
}

} // namespace

void URampLabSimulationSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    double RequestedCaptureMultiplier = CaptureMultiplier;
    bCaptureQA = FParse::Param(FCommandLine::Get(), TEXT("RampLabCapture"));
    bFleetValidation=FParse::Param(FCommandLine::Get(),TEXT("RampLabFleetValidation"));
    bGoal12ClosureValidation=FParse::Param(FCommandLine::Get(),TEXT("RampLabGoal12ClosureValidation"));
    bGoal12RecoveryValidation=FParse::Param(FCommandLine::Get(),TEXT("RampLabGoal12RecoveryValidation"));
    bGoal13DispatchValidation=FParse::Param(FCommandLine::Get(),TEXT("RampLabGoal13DispatchValidation"));
    bGoal13ReassignmentValidation=FParse::Param(FCommandLine::Get(),TEXT("RampLabGoal13ReassignmentValidation"));
    bTurnaroundValidation=FParse::Param(FCommandLine::Get(),TEXT("RampLabTurnaroundValidation"));
    if (FParse::Param(FCommandLine::Get(), TEXT("RampLabSurfaceTraffic"))) {
        SelectedScenarioKey = TEXT("surface_traffic");
        PlaybackSpeed = 10.0;
    } else if (FParse::Param(FCommandLine::Get(), TEXT("RampLabSurfaceDisruption"))) {
        SelectedScenarioKey = TEXT("surface_traffic_disrupted");
        PlaybackSpeed = 10.0;
    }
    if (FParse::Param(FCommandLine::Get(), TEXT("RampLabMixedRunwayValidation"))) {
        SelectedScenarioKey = TEXT("mixed_runway_operations"); PlaybackSpeed = 20.0;
    } else if (FParse::Param(FCommandLine::Get(), TEXT("RampLabMixedRunwayDisruption"))) {
        SelectedScenarioKey = TEXT("mixed_runway_disrupted"); PlaybackSpeed = 20.0;
    }
    if(bGoal13ReassignmentValidation){SelectedScenarioKey=TEXT("autonomy_dispatch_reassignment");PlaybackSpeed=60.0;}
    else if(bGoal13DispatchValidation){SelectedScenarioKey=TEXT("autonomy_dispatch_dynamic");PlaybackSpeed=60.0;}
    else if(bTurnaroundValidation){SelectedScenarioKey=TEXT("turnaround_flight_bank_outage");PlaybackSpeed=20.0;}
    else if(bGoal12RecoveryValidation){SelectedScenarioKey=TEXT("autonomy_fleet_deadlock");PlaybackSpeed=10.0;}
    else if(bGoal12ClosureValidation){SelectedScenarioKey=TEXT("autonomy_fleet_dynamic_closure");PlaybackSpeed=10.0;}
    else if(bFleetValidation){SelectedScenarioKey=TEXT("autonomy_fleet");PlaybackSpeed=10.0;}
    if (bCaptureQA) {
        CaptureWarmupRemaining = 15.0;
        if (FParse::Value(FCommandLine::Get(), TEXT("RampLabCaptureMultiplier="), RequestedCaptureMultiplier)) {
            CaptureMultiplier = FMath::Clamp(RequestedCaptureMultiplier, 1.0, 20.0);
        }
        UE_LOG(LogRampLab, Display, TEXT("Capture QA acceleration enabled: %.0fx operator playback x %.0fx multiplier"),
            PlaybackSpeed, CaptureMultiplier);
    }
    bControlCheck = FParse::Param(FCommandLine::Get(), TEXT("RampLabControlCheck"));
    if (FParse::Param(FCommandLine::Get(), TEXT("RampLabSensorValidation"))) {
        SelectedScenarioKey = TEXT("autonomy_sensor_validation");
        PlaybackSpeed = 1.0;
    }
    bDemoMode = FParse::Param(FCommandLine::Get(), TEXT("RampLabDemo"))
        || FParse::Param(FCommandLine::Get(), TEXT("RampLabCapture"));
    BuildScenarioComparison();
    LoadSelectedScenario();
}

void URampLabSimulationSubsystem::Deinitialize()
{
    if (ControlPanel.IsValid() && GEngine != nullptr && GEngine->GameViewport != nullptr) {
        GEngine->GameViewport->RemoveViewportWidgetContent(ControlPanel.ToSharedRef());
    }
    ControlPanel.Reset();
    Snapshot.Reset();
    AutonomySnapshot.Reset();
    AutonomySimulation.Reset();
    FleetSimulation.Reset();
    FleetSnapshots.clear();
    AutonomyController.Reset();
    Simulation.Reset();
    Super::Deinitialize();
}

void URampLabSimulationSubsystem::Tick(float DeltaTime)
{
    if (bControlCheck) RunControlCheck(DeltaTime);
    if (CaptureWarmupRemaining > 0.0 && bViewerReady) {
        CaptureWarmupRemaining = FMath::Max(0.0, CaptureWarmupRemaining - DeltaTime);
        return;
    }
    if (FleetSimulation != nullptr) {
        if (!bPlaying || FleetSimulation->finished()) return;
        const double FrameBudget=static_cast<double>(DeltaTime)*PlaybackSpeed*CaptureMultiplier;AutonomyAccumulator+=FrameBudget;constexpr double FixedStep=0.02;
        while(AutonomyAccumulator+1e-9>=FixedStep&&!FleetSimulation->finished()){(void)FleetSimulation->advance();AutonomyAccumulator-=FixedStep;FleetSnapshots=FleetSimulation->snapshots();}
        if(!FleetSnapshots.empty())PlaybackSeconds=FleetSnapshots.front().autonomy.timestamp_s;
        FleetMetricsSnapshot=FleetSimulation->result();
        while(FleetEventCount<FleetMetricsSnapshot.events.size()){
            const auto& Event=FleetMetricsSnapshot.events[FleetEventCount++];
            const FString Message=FString::Printf(TEXT("%6.2f  %s  %s%s%s"),Event.time_s,
                UTF8_TO_TCHAR(airside::autonomy::to_string(Event.kind).c_str()),
                UTF8_TO_TCHAR(Event.vehicle.value.c_str()),
                Event.other.value.empty()?TEXT(""):TEXT(" -> "),
                Event.other.value.empty()?TEXT(""):UTF8_TO_TCHAR(Event.other.value.c_str()));
            FString RecoveryDetail;
            if(Event.kind==airside::autonomy::TrafficEventKind::RetreatStarted)
                RecoveryDetail=FString::Printf(TEXT("  target %.1f, %.1f"),Event.target.x_m,Event.target.y_m);
            else if(Event.kind==airside::autonomy::TrafficEventKind::RetreatProgress||Event.kind==airside::autonomy::TrafficEventKind::RetreatCompleted)
                RecoveryDetail=FString::Printf(TEXT("  %.1f m at %.1f, %.1f"),Event.progress_m,Event.position.x_m,Event.position.y_m);
            RecentEvents.Add(Message+TEXT("  ")+UTF8_TO_TCHAR(Event.resource.c_str())+RecoveryDetail);
            const bool bRecoveryLifecycle = Event.kind==airside::autonomy::TrafficEventKind::RetreatSelected
                || Event.kind==airside::autonomy::TrafficEventKind::RetreatStarted
                || Event.kind==airside::autonomy::TrafficEventKind::RetreatCompleted
                || Event.kind==airside::autonomy::TrafficEventKind::RetreatResourceReleased
                || Event.kind==airside::autonomy::TrafficEventKind::MissionResumed
                || Event.kind==airside::autonomy::TrafficEventKind::RetreatFailed;
            RecentEventIsRecoveryLifecycle.Add(bRecoveryLifecycle);
            if(RecentEvents.Num()>8){
                int32 EvictIndex=RecentEventIsRecoveryLifecycle.IndexOfByKey(false);
                if(EvictIndex==INDEX_NONE)EvictIndex=0;
                RecentEvents.RemoveAt(EvictIndex);RecentEventIsRecoveryLifecycle.RemoveAt(EvictIndex);
            }
        }
        while(FleetDispatchEventCount<FleetMetricsSnapshot.dispatch.events.size()){
            const auto& Event=FleetMetricsSnapshot.dispatch.events[FleetDispatchEventCount++];
            if(Event.kind!=airside::autonomy::DispatchEventKind::RequestReleased&&
               Event.kind!=airside::autonomy::DispatchEventKind::Assigned&&
               Event.kind!=airside::autonomy::DispatchEventKind::Reassigned&&
               Event.kind!=airside::autonomy::DispatchEventKind::Completed&&
               Event.kind!=airside::autonomy::DispatchEventKind::AgingApplied&&
               Event.kind!=airside::autonomy::DispatchEventKind::Failed)continue;
            const FString Message=FString::Printf(TEXT("%6.2f  %s  %s%s%s  priority %lld  %s"),Event.time_s,
                UTF8_TO_TCHAR(airside::autonomy::to_string(Event.kind).c_str()),
                UTF8_TO_TCHAR(Event.request.value.c_str()),Event.vehicle.value.empty()?TEXT(""):TEXT(" -> "),
                UTF8_TO_TCHAR(Event.vehicle.value.c_str()),static_cast<long long>(Event.effective_priority),
                UTF8_TO_TCHAR(Event.detail.c_str()));
            RecentEvents.Add(Message);RecentEventIsRecoveryLifecycle.Add(true);
            if(RecentEvents.Num()>8){int32 EvictIndex=RecentEventIsRecoveryLifecycle.IndexOfByKey(false);if(EvictIndex==INDEX_NONE)EvictIndex=0;RecentEvents.RemoveAt(EvictIndex);RecentEventIsRecoveryLifecycle.RemoveAt(EvictIndex);}
        }
        if(FleetSimulation->finished()){
            bPlaying=false;
            const auto& M=FleetMetricsSnapshot;
            FinalResultText=FString::Printf(
                TEXT("FLEET %llu vehicles\nMissions %llu / %llu\nWaiting %.1f s\nReservations %llu / contention %llu / outstanding %llu\nDeadlocks %llu / resolved %llu\nRecoveries %llu / retreats %llu / reroutes %llu\nCollisions %llu\nMinimum separation %.2f m"),
                static_cast<unsigned long long>(M.vehicle_count),static_cast<unsigned long long>(M.missions_completed),
                static_cast<unsigned long long>(M.missions_attempted),M.traffic_waiting_time_s,
                static_cast<unsigned long long>(M.reservation_requests),static_cast<unsigned long long>(M.reservation_contentions),
                static_cast<unsigned long long>(M.outstanding_reservations),static_cast<unsigned long long>(M.deadlock_count),
                static_cast<unsigned long long>(M.deadlocks_resolved),static_cast<unsigned long long>(M.recovery_attempts),
                static_cast<unsigned long long>(M.retreat_count),static_cast<unsigned long long>(M.reroutes),
                static_cast<unsigned long long>(M.collisions),M.minimum_separation_m);
            StatusText=TEXT("Fleet scenario finished");
            UE_LOG(LogRampLab,Display,TEXT("Fleet runtime validation: vehicles=%llu completed=%llu/%llu timeouts=%llu waiting_s=%.2f reservations=%llu contentions=%llu outstanding=%llu deadlocks=%llu resolved=%llu recoveries=%llu retreats=%llu reroutes=%llu collisions=%llu minimum_separation_m=%.3f digest=%llu"),
                static_cast<unsigned long long>(M.vehicle_count),static_cast<unsigned long long>(M.missions_completed),
                static_cast<unsigned long long>(M.missions_attempted),static_cast<unsigned long long>(M.safe_timeouts),
                M.traffic_waiting_time_s,static_cast<unsigned long long>(M.reservation_requests),
                static_cast<unsigned long long>(M.reservation_contentions),static_cast<unsigned long long>(M.outstanding_reservations),
                static_cast<unsigned long long>(M.deadlock_count),static_cast<unsigned long long>(M.deadlocks_resolved),
                static_cast<unsigned long long>(M.recovery_attempts),static_cast<unsigned long long>(M.retreat_count),
                static_cast<unsigned long long>(M.reroutes),static_cast<unsigned long long>(M.collisions),M.minimum_separation_m,
                static_cast<unsigned long long>(M.deterministic_digest));
            if(bGoal12RecoveryValidation||bGoal13DispatchValidation||bGoal13ReassignmentValidation)UE_LOG(LogRampLab,Display,TEXT("Recent Events panel: %s"),*FString::Join(RecentEvents,TEXT(" | ")));
            if(bGoal13DispatchValidation||bGoal13ReassignmentValidation)UE_LOG(LogRampLab,Display,TEXT("Dispatch runtime: requests=%llu completed=%llu failed=%llu assignments=%llu reassignments=%llu aging=%llu unfinished=%llu queue_wait_avg_s=%.3f queue_wait_max_s=%.3f"),
                static_cast<unsigned long long>(M.dispatch.requests_created),static_cast<unsigned long long>(M.dispatch.requests_completed),
                static_cast<unsigned long long>(M.dispatch.requests_failed),static_cast<unsigned long long>(M.dispatch.assignments),
                static_cast<unsigned long long>(M.dispatch.reassignments),static_cast<unsigned long long>(M.dispatch.aging_activations),
                static_cast<unsigned long long>(M.dispatch.unfinished_requests),M.dispatch.average_queue_wait_s,M.dispatch.maximum_queue_wait_s);
            if(bFleetValidation||bGoal12ClosureValidation||bGoal12RecoveryValidation||bGoal13DispatchValidation||bGoal13ReassignmentValidation)FPlatformMisc::RequestExit(false);
        }
        return;
    }
    if (AutonomySimulation != nullptr) {
        if (!bPlaying || AutonomySimulation->finished()) return;
        const double FrameBudget = bCaptureQA
            ? FMath::Min(static_cast<double>(DeltaTime) * PlaybackSpeed * CaptureMultiplier, 0.2)
            : static_cast<double>(DeltaTime) * PlaybackSpeed * CaptureMultiplier;
        AutonomyAccumulator += FrameBudget;
        constexpr double FixedStep = 0.02;
        while (AutonomyAccumulator + 1e-9 >= FixedStep && !AutonomySimulation->finished()) {
            (void)AutonomySimulation->advance(*AutonomyController);
            AutonomyAccumulator -= FixedStep;
            AutonomySnapshot = AutonomySimulation->snapshot();
        }
        if (AutonomySnapshot.IsSet()) PlaybackSeconds = AutonomySnapshot->timestamp_s;
        if (AutonomySimulation->finished()) {
            bPlaying = false;
            const auto Run = AutonomySimulation->result();
            FinalResultText = FString::Printf(TEXT("AUTONOMY %s\nCompletion %.2f s\nDistance %.2f m\nMean route error %.2f m\nMinimum clearance %.2f m\nEmergency stops %llu\nCollisions %llu"),
                UTF8_TO_TCHAR(airside::autonomy::to_string(Run.metrics.result).c_str()), Run.metrics.completion_time_s,
                Run.metrics.distance_traveled_m, Run.metrics.mean_route_error_m, Run.metrics.minimum_obstacle_clearance_m,
                static_cast<unsigned long long>(Run.metrics.emergency_stops), static_cast<unsigned long long>(Run.metrics.collision_count));
            StatusText = TEXT("Autonomy mission finished");
            UE_LOG(LogRampLab, Display, TEXT("RampLab autonomy completed: result=%s time=%.2f distance=%.2f collisions=%llu"),
                UTF8_TO_TCHAR(airside::autonomy::to_string(Run.metrics.result).c_str()), Run.metrics.completion_time_s,
                Run.metrics.distance_traveled_m, static_cast<unsigned long long>(Run.metrics.collision_count));
        }
        return;
    }
    if (Simulation != nullptr && Simulation->finished()) {
        if (bDemoMode && !bDemoAdvancedToHighCapacity && SelectedScenarioKey == TEXT("baseline")) {
            DemoTransitionWallSeconds += DeltaTime;
            if (DemoTransitionWallSeconds >= 2.0) {
                bDemoAdvancedToHighCapacity = true;
                SelectScenario(TEXT("high_capacity"));
            }
        }
        if (bDemoMode && bDemoAdvancedToHighCapacity && !bDemoAdvancedToAutonomy && SelectedScenarioKey == TEXT("high_capacity")) {
            DemoTransitionWallSeconds += DeltaTime;
            if (DemoTransitionWallSeconds >= 2.0) { bDemoAdvancedToAutonomy = true; SelectScenario(TEXT("autonomy_tug")); }
        }
        return;
    }
    if (!bPlaying || Simulation == nullptr) return;

    PlaybackSeconds += static_cast<double>(DeltaTime) * PlaybackSpeed * CaptureMultiplier;
    const auto Target = airside::SimTime{static_cast<airside::SimTime::rep>(PlaybackSeconds)};
    bool bAdvanced = false;
    while (const auto Next = Simulation->next_event_time()) {
        if (*Next > Target) break;
        if (!Simulation->advance()) break;
        bAdvanced = true;
    }

    if (bAdvanced) ReconcileSnapshot();
    if (Simulation->finished()) {
        PlaybackSeconds = static_cast<double>(Simulation->current_time().count());
        bPlaying = false;
        StatusText = TEXT("Simulation finished");
        if (!bCompletionReported) {
            const auto Result = Simulation->result();
            UE_LOG(LogRampLab, Display,
                TEXT("RampLab completed: duration=%lld sec, average turnaround=%.1f min, delayed=%llu/%llu, fuel utilization=%.1f%%, baggage utilization=%.1f%%"),
                Result.simulated_duration.count(),
                Result.metrics.average_turnaround_seconds / 60.0,
                static_cast<unsigned long long>(Result.metrics.delayed_aircraft),
                static_cast<unsigned long long>(Result.metrics.aircraft.size()),
                Result.metrics.fuel_utilization * 100.0,
                Result.metrics.baggage_utilization * 100.0);
            for (const auto& Aircraft : Result.metrics.aircraft) {
                UE_LOG(LogRampLab, Display,
                    TEXT("RampLab aircraft: %s turnaround=%.1f min, delay=%.1f min, service wait=%.1f min"),
                    UTF8_TO_TCHAR(Aircraft.flight_number.c_str()),
                    Aircraft.turnaround.count() / 60.0,
                    Aircraft.departure_delay.count() / 60.0,
                    Aircraft.service_waiting.count() / 60.0);
            }
            if (bTurnaroundValidation) {
                UE_LOG(LogRampLab, Display,
                    TEXT("Turnaround runtime validation: completed=%llu/%llu failed=%llu task_reassignments=%llu fleet_reassignments=%llu service_requests=%llu/%llu/%llu collisions=%llu minimum_separation_m=%.3f reservation_requests=%llu contentions=%llu outstanding=%llu unresolved=%llu"),
                    static_cast<unsigned long long>(Result.metrics.completed_turnarounds),
                    static_cast<unsigned long long>(Result.metrics.total_turnarounds),
                    static_cast<unsigned long long>(Result.metrics.failed_or_timed_out_turnarounds),
                    static_cast<unsigned long long>(Result.metrics.task_reassignments),
                    static_cast<unsigned long long>(Result.metrics.fleet_reassignments),
                    static_cast<unsigned long long>(Result.metrics.fleet_requests_created),
                    static_cast<unsigned long long>(Result.metrics.fleet_requests_completed),
                    static_cast<unsigned long long>(Result.metrics.fleet_requests_failed),
                    static_cast<unsigned long long>(Result.metrics.fleet_collisions), Result.metrics.fleet_minimum_separation_m,
                    static_cast<unsigned long long>(Result.metrics.fleet_reservation_requests),
                    static_cast<unsigned long long>(Result.metrics.fleet_reservation_contentions),
                    static_cast<unsigned long long>(Result.metrics.fleet_outstanding_reservations),
                    static_cast<unsigned long long>(Result.metrics.unresolved_service_requests));
                const auto OutageEvent = std::ranges::find_if(Result.events, [](const auto& Event) {
                    return Event.type == airside::SimulationEventType::TurnaroundVehicleUnavailable;
                });
                const auto ReassignmentEvent = std::ranges::find_if(Result.events, [](const auto& Event) {
                    return Event.type == airside::SimulationEventType::TurnaroundTaskReassigned;
                });
                if (OutageEvent != Result.events.end()) {
                    UE_LOG(LogRampLab, Display, TEXT("Turnaround outage observed: time=%lld vehicle=%u task=%u"),
                        OutageEvent->timestamp.count(), OutageEvent->vehicle ? OutageEvent->vehicle->value() : 0,
                        OutageEvent->task ? OutageEvent->task->value() : 0);
                }
                if (ReassignmentEvent != Result.events.end()) {
                    UE_LOG(LogRampLab, Display, TEXT("Turnaround reassignment observed: time=%lld replacement_vehicle=%u task=%u"),
                        ReassignmentEvent->timestamp.count(), ReassignmentEvent->vehicle ? ReassignmentEvent->vehicle->value() : 0,
                        ReassignmentEvent->task ? ReassignmentEvent->task->value() : 0);
                }
                const auto TaskCompletions = std::ranges::count_if(Result.events, [](const auto& Event) {
                    return Event.type == airside::SimulationEventType::TurnaroundTaskCompleted;
                });
                const auto Departures = std::ranges::count_if(Result.events, [](const auto& Event) {
                    return Event.type == airside::SimulationEventType::AircraftDeparted;
                });
                UE_LOG(LogRampLab, Display,
                    TEXT("Turnaround event totals: task_completions=%llu departures=%llu outage_observed=%d reassignment_observed=%d"),
                    static_cast<unsigned long long>(TaskCompletions), static_cast<unsigned long long>(Departures),
                    OutageEvent != Result.events.end(), ReassignmentEvent != Result.events.end());
                UE_LOG(LogRampLab, Display, TEXT("Turnaround Recent Events panel: %s"), *FString::Join(RecentEvents, TEXT(" | ")));
            }
            bCompletionReported = true;
            FinalResultText = FormatResult(ScenarioName.ToUpper(), Result);
            if (Result.metrics.surface_total_aircraft > 0) {
                UE_LOG(LogRampLab, Display,
                    TEXT("Surface runtime validation: arrivals=%llu departures=%llu/%llu runway_operations=%llu queue_max=%llu arrival_wait_s=%lld departure_wait_s=%lld occupied_s=%lld utilization=%.3f arrival_taxi_s=%lld arrival_taxi_m=%.1f departure_taxi_s=%lld departure_taxi_m=%.1f throughput_per_hour=%.3f reroutes=%llu waits=%llu wait_seconds=%lld taxi_seconds=%lld taxi_distance_m=%.1f runway_queue_seconds=%lld safe_failures=%llu aircraft_collisions=%llu aircraft_ground_collisions=%llu minimum_aircraft_separation_m=%.3f minimum_aircraft_ground_separation_m=%.3f"),
                    static_cast<unsigned long long>(Result.metrics.surface_arrived_aircraft),
                    static_cast<unsigned long long>(Result.metrics.surface_departed_aircraft),
                    static_cast<unsigned long long>(Result.metrics.surface_total_aircraft),
                    static_cast<unsigned long long>(Result.metrics.runway_operations_completed),
                    static_cast<unsigned long long>(Result.metrics.maximum_runway_queue_depth),
                    static_cast<long long>(Result.metrics.arrival_runway_wait_seconds),
                    static_cast<long long>(Result.metrics.departure_runway_wait_seconds),
                    static_cast<long long>(Result.metrics.runway_occupied_seconds), Result.metrics.runway_utilization,
                    static_cast<long long>(Result.metrics.arrival_taxi_seconds), Result.metrics.arrival_taxi_distance_m,
                    static_cast<long long>(Result.metrics.departure_taxi_seconds), Result.metrics.departure_taxi_distance_m,
                    Result.metrics.surface_departure_throughput_per_hour,
                    static_cast<unsigned long long>(Result.metrics.surface_reroutes),
                    static_cast<unsigned long long>(Result.metrics.surface_wait_events),
                    static_cast<long long>(Result.metrics.surface_wait_seconds),
                    static_cast<long long>(Result.metrics.surface_taxi_seconds), Result.metrics.surface_taxi_distance_m,
                    static_cast<long long>(Result.metrics.runway_queue_seconds),
                    static_cast<unsigned long long>(Result.metrics.surface_safe_failures),
                    static_cast<unsigned long long>(Result.metrics.surface_aircraft_aircraft_collisions),
                    static_cast<unsigned long long>(Result.metrics.surface_aircraft_ground_collisions),
                    Result.metrics.minimum_aircraft_separation_m,
                    Result.metrics.minimum_aircraft_ground_separation_m);
                FinalResultText += FString::Printf(
                    TEXT("\n\nSURFACE OPERATIONS\nDeparted  %llu / %llu\nThroughput  %.2f departures/hour\nReroutes  %llu\nTraffic waits  %llu  /  %lld s\nTaxi  %lld s  /  %.1f m\nRunway queue  %lld s\nSafe failures  %llu\nAircraft collisions  %llu\nAircraft / vehicle collisions  %llu\nMinimum aircraft separation  %.2f m\nMinimum aircraft / vehicle separation  %.2f m"),
                    static_cast<unsigned long long>(Result.metrics.surface_departed_aircraft),
                    static_cast<unsigned long long>(Result.metrics.surface_total_aircraft),
                    Result.metrics.surface_departure_throughput_per_hour,
                    static_cast<unsigned long long>(Result.metrics.surface_reroutes),
                    static_cast<unsigned long long>(Result.metrics.surface_wait_events),
                    static_cast<long long>(Result.metrics.surface_wait_seconds),
                    static_cast<long long>(Result.metrics.surface_taxi_seconds), Result.metrics.surface_taxi_distance_m,
                    static_cast<long long>(Result.metrics.runway_queue_seconds),
                    static_cast<unsigned long long>(Result.metrics.surface_safe_failures),
                    static_cast<unsigned long long>(Result.metrics.surface_aircraft_aircraft_collisions),
                    static_cast<unsigned long long>(Result.metrics.surface_aircraft_ground_collisions),
                    Result.metrics.minimum_aircraft_separation_m,
                    Result.metrics.minimum_aircraft_ground_separation_m);
            }
        }
    }
}

TStatId URampLabSimulationSubsystem::GetStatId() const
{
    RETURN_QUICK_DECLARE_CYCLE_STAT(URampLabSimulationSubsystem, STATGROUP_Tickables);
}

bool URampLabSimulationSubsystem::IsTickable() const
{
    return !HasAnyFlags(RF_ClassDefaultObject) && bViewerReady && Simulation != nullptr;
}

void URampLabSimulationSubsystem::on_event(const airside::SimulationEventRecord& Event) noexcept
{
    try {
        const FString Message = UTF8_TO_TCHAR(airside::format_event(Event).c_str());
        RecentEvents.Add(Message);
        RecentEventIsRecoveryLifecycle.Add(false);
        constexpr int32 MaxEvents = 8;
        if (RecentEvents.Num() > MaxEvents) {
            RecentEvents.RemoveAt(0, RecentEvents.Num() - MaxEvents);
            RecentEventIsRecoveryLifecycle.RemoveAt(0, RecentEventIsRecoveryLifecycle.Num() - MaxEvents);
        }
        UE_LOG(LogRampLab, Log, TEXT("%s"), *Message);
    } catch (...) {
        UE_LOG(LogRampLab, Warning, TEXT("Failed to format simulation event %llu"), Event.sequence);
    }
}

void URampLabSimulationSubsystem::TogglePlaying()
{
    if (FleetSimulation != nullptr && !FleetSimulation->finished()) { bPlaying = !bPlaying; return; }
    if (AutonomySimulation != nullptr && !AutonomySimulation->finished()) { bPlaying = !bPlaying; return; }
    if (Simulation != nullptr && !Simulation->finished()) bPlaying = !bPlaying;
}

void URampLabSimulationSubsystem::ResetSimulation()
{
    LoadSelectedScenario();
}

void URampLabSimulationSubsystem::SelectScenario(const FString& ScenarioKey)
{
    if (ScenarioKey != TEXT("baseline") && ScenarioKey != TEXT("high_capacity") &&
        ScenarioKey != TEXT("autonomy_tug") && ScenarioKey != TEXT("autonomy_sensor_validation") &&
        ScenarioKey != TEXT("turnaround_normal") && ScenarioKey != TEXT("turnaround_contention") &&
        ScenarioKey != TEXT("turnaround_disrupted") && ScenarioKey != TEXT("turnaround_flight_bank") &&
        ScenarioKey != TEXT("turnaround_flight_bank_disrupted") &&
        ScenarioKey != TEXT("mixed_runway_operations") && ScenarioKey != TEXT("mixed_runway_disrupted")) return;
    SelectedScenarioKey = ScenarioKey;
    LoadSelectedScenario();
}

void URampLabSimulationSubsystem::SelectEntityKind(const FString& Kind)
{
    if (!Snapshot.IsSet()) return;
    if (Kind != SelectedEntityKind) SelectedEntityId = 0;
    const auto& Value = Snapshot.GetValue();
    if (Kind == TEXT("aircraft") && !Value.aircraft.empty()) {
        SelectedEntityKind = Kind;
        const auto Match = std::ranges::find_if(Value.aircraft, [this](const auto& Item) { return Item.id.value() > SelectedEntityId; });
        SelectedEntityId = (Match == Value.aircraft.end() ? Value.aircraft.front() : *Match).id.value();
    } else if (Kind == TEXT("vehicle") && !Value.vehicles.empty()) {
        SelectedEntityKind = Kind;
        const auto Match = std::ranges::find_if(Value.vehicles, [this](const auto& Item) { return Item.id.value() > SelectedEntityId; });
        SelectedEntityId = (Match == Value.vehicles.end() ? Value.vehicles.front() : *Match).id.value();
    } else if (Kind == TEXT("gate") && !Value.gates.empty()) {
        SelectedEntityKind = Kind;
        const auto Match = std::ranges::find_if(Value.gates, [this](const auto& Item) { return Item.id.value() > SelectedEntityId; });
        SelectedEntityId = (Match == Value.gates.end() ? Value.gates.front() : *Match).id.value();
    }
}

void URampLabSimulationSubsystem::SetPlaybackSpeed(double NewSpeed)
{
    if (NewSpeed == 1.0 || NewSpeed == 5.0 || NewSpeed == 10.0 || NewSpeed == 20.0) {
        PlaybackSpeed = NewSpeed;
    }
}

void URampLabSimulationSubsystem::AttachControlPanel()
{
    if (ControlPanel.IsValid() || GEngine == nullptr || GEngine->GameViewport == nullptr) return;
    ControlPanel =
        SNew(SOverlay)
        + SOverlay::Slot()
        .HAlign(HAlign_Left)
        .VAlign(VAlign_Top)
        .Padding(12.0f)
        [
            SNew(SBox)
            .WidthOverride(455.0f)
            .HeightOverride(850.0f)
            [
                SNew(SRampLabControlPanel).Subsystem(this)
            ]
        ];
    GEngine->GameViewport->AddViewportWidgetContent(ControlPanel.ToSharedRef(), 100);
    ResetSimulation();
    bViewerReady = true;
}

bool URampLabSimulationSubsystem::IsFinished() const noexcept
{
    if (FleetSimulation != nullptr) return FleetSimulation->finished();
    if (AutonomySimulation != nullptr) return AutonomySimulation->finished();
    return Simulation != nullptr && Simulation->finished();
}

airside::SimTime URampLabSimulationSubsystem::GetPlaybackTime() const noexcept
{
    return airside::SimTime{static_cast<airside::SimTime::rep>(PlaybackSeconds)};
}

const airside::SimulationSnapshot* URampLabSimulationSubsystem::GetSnapshot() const noexcept
{
    return Snapshot.IsSet() ? &Snapshot.GetValue() : nullptr;
}

const airside::autonomy::AutonomySnapshot* URampLabSimulationSubsystem::GetAutonomySnapshot() const noexcept
{
    return AutonomySnapshot.IsSet() ? &AutonomySnapshot.GetValue() : nullptr;
}

const airside::autonomy::AutonomyScenario* URampLabSimulationSubsystem::GetAutonomyScenario() const noexcept
{
    return AutonomySimulation == nullptr ? nullptr : &AutonomySimulation->scenario();
}

bool URampLabSimulationSubsystem::LoadSelectedScenario()
{
    try {
        AutonomySimulation.Reset();
        FleetSimulation.Reset(); FleetSnapshots.clear();
        FleetMetricsSnapshot={};FleetEventCount=0;FleetDispatchEventCount=0;
        RecentEventIsRecoveryLifecycle.Reset();
        AutonomyController.Reset();
        AutonomySnapshot.Reset();
        AutonomyAccumulator = 0.0;
        const FString Path = FindScenarioPath(SelectedScenarioKey + TEXT(".yaml"));
        if (Path.IsEmpty()) {
            StatusText = SelectedScenarioKey + TEXT(".yaml was not found");
            UE_LOG(LogRampLab, Error, TEXT("%s"), *StatusText);
            return false;
        }

        if (SelectedScenarioKey == TEXT("autonomy_tug") || SelectedScenarioKey == TEXT("autonomy_sensor_validation")) {
            auto Scenario = airside::autonomy::load_scenario(std::filesystem::path{*Path});
            ScenarioName = UTF8_TO_TCHAR(Scenario.name.c_str());
            Seed = Scenario.default_seed;
            const double SafetyRange = Scenario.safety_stop_range_m;
            auto AirportScenario = Scenario.airport;
            Simulation = MakeUnique<airside::Simulation>(std::move(AirportScenario), Seed);
            ReconcileSnapshot();
            AutonomySimulation = MakeUnique<airside::autonomy::AutonomySimulation>(std::move(Scenario), Seed);
            AutonomyController = MakeUnique<airside::autonomy::ReferenceController>(SafetyRange);
            AutonomySnapshot = AutonomySimulation->snapshot();
            RecentEvents.Reset();RecentEventIsRecoveryLifecycle.Reset(); PlaybackSeconds = 0.0; bPlaying = true; bCompletionReported = false;
            FinalResultText.Reset(); StatusText = FString::Printf(TEXT("Loaded A* Depot to Gate A2 autonomy mission with seed %llu"), Seed);
            UE_LOG(LogRampLab, Display, TEXT("%s"), *StatusText);
            return true;
        }
        if(SelectedScenarioKey==TEXT("autonomy_fleet")||SelectedScenarioKey==TEXT("autonomy_fleet_fault")||SelectedScenarioKey==TEXT("autonomy_fleet_dynamic_closure")||SelectedScenarioKey==TEXT("autonomy_fleet_deadlock")||SelectedScenarioKey==TEXT("autonomy_dispatch_dynamic")||SelectedScenarioKey==TEXT("autonomy_dispatch_fairness")||SelectedScenarioKey==TEXT("autonomy_dispatch_reassignment")){
            auto Fleet=airside::autonomy::load_fleet_scenario(std::filesystem::path{*Path});ScenarioName=UTF8_TO_TCHAR(Fleet.name.c_str());Seed=Fleet.default_seed;auto Airport=Fleet.vehicle_scenario.airport;Simulation=MakeUnique<airside::Simulation>(std::move(Airport),Seed);ReconcileSnapshot();FleetSimulation=MakeUnique<airside::autonomy::FleetSimulation>(std::move(Fleet),Seed);FleetSnapshots=FleetSimulation->snapshots();FleetMetricsSnapshot=FleetSimulation->result();FleetEventCount=0;FleetDispatchEventCount=0;RecentEvents.Reset();RecentEventIsRecoveryLifecycle.Reset();PlaybackSeconds=0.0;bPlaying=true;bCompletionReported=false;FinalResultText.Reset();StatusText=FString::Printf(TEXT("Loaded %llu-vehicle deterministic fleet seed %llu"),static_cast<unsigned long long>(FleetSnapshots.size()),Seed);UE_LOG(LogRampLab,Display,TEXT("%s"),*StatusText);return true;
        }
        auto Scenario = airside::load_scenario(std::filesystem::path{*Path});
        ScenarioName = UTF8_TO_TCHAR(Scenario.name.c_str());
        Seed = airside::resolve_seed(Scenario, std::nullopt);
        RecentEvents.Reset();RecentEventIsRecoveryLifecycle.Reset();
        PlaybackSeconds = 0.0;
        bPlaying = true;
        bCompletionReported = false;
        DemoTransitionWallSeconds = 0.0;
        FinalResultText.Reset();
        Simulation = MakeUnique<airside::Simulation>(std::move(Scenario), Seed);
        Simulation->add_event_sink(*this);

        while (Simulation->next_event_time() && *Simulation->next_event_time() == airside::SimTime::zero()) {
            if (!Simulation->advance()) break;
        }
        ReconcileSnapshot();
        if (!Snapshot->vehicles.empty()) {
            SelectedEntityKind = TEXT("vehicle");
            SelectedEntityId = Snapshot->vehicles.front().id.value();
        }
        StatusText = FString::Printf(TEXT("Loaded %s with seed %llu"), *ScenarioName, Seed);
        UE_LOG(LogRampLab, Display, TEXT("%s from %s"), *StatusText, *Path);
        return true;
    } catch (const std::exception& Error) {
        Simulation.Reset();
        Snapshot.Reset();
        AutonomySimulation.Reset();
        FleetSimulation.Reset(); FleetSnapshots.clear();
        AutonomyController.Reset();
        AutonomySnapshot.Reset();
        StatusText = FString::Printf(TEXT("RampLab initialization failed: %s"), UTF8_TO_TCHAR(Error.what()));
        UE_LOG(LogRampLab, Error, TEXT("%s"), *StatusText);
        return false;
    }
}

FString URampLabSimulationSubsystem::FindScenarioPath(const FString& Filename) const
{
    const TArray<FString> Candidates{
        FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT(".."), TEXT(".."), TEXT("scenarios"), Filename)),
        FPaths::ConvertRelativePathToFull(FPaths::Combine(FPlatformProcess::BaseDir(), TEXT("Scenarios"), Filename)),
    };
    for (const auto& Candidate : Candidates) {
        if (FPaths::FileExists(Candidate)) return Candidate;
    }
    return {};
}

void URampLabSimulationSubsystem::BuildScenarioComparison()
{
    try {
        const FString BaselinePath = FindScenarioPath(TEXT("baseline.yaml"));
        const FString CapacityPath = FindScenarioPath(TEXT("high_capacity.yaml"));
        if (BaselinePath.IsEmpty() || CapacityPath.IsEmpty()) return;
        auto BaselineScenario = airside::load_scenario(std::filesystem::path{*BaselinePath});
        auto CapacityScenario = airside::load_scenario(std::filesystem::path{*CapacityPath});
        const uint64 BaselineSeed = airside::resolve_seed(BaselineScenario, std::nullopt);
        const uint64 CapacitySeed = airside::resolve_seed(CapacityScenario, std::nullopt);
        airside::Simulation Baseline(std::move(BaselineScenario), BaselineSeed);
        airside::Simulation Capacity(std::move(CapacityScenario), CapacitySeed);
        const auto BaselineResult = Baseline.run();
        const auto CapacityResult = Capacity.run();
        ComparisonText = FormatResult(TEXT("BASELINE"), BaselineResult)
            + TEXT("\n\n") + FormatResult(TEXT("HIGH CAPACITY"), CapacityResult);
        UE_LOG(LogRampLab, Display, TEXT("Scenario comparison generated from YAML engine runs:\n%s"), *ComparisonText);
    } catch (const std::exception& Error) {
        UE_LOG(LogRampLab, Error, TEXT("Scenario comparison failed: %s"), UTF8_TO_TCHAR(Error.what()));
    }
}

FString URampLabSimulationSubsystem::GetSelectedEntityText() const
{
    if (!Snapshot.IsSet()) return TEXT("No entity selected");
    const auto& Value = Snapshot.GetValue();
    if (SelectedEntityKind == TEXT("aircraft")) {
        const auto Match = std::ranges::find(Value.aircraft, SelectedEntityId, [](const auto& Item) { return Item.id.value(); });
        if (Match != Value.aircraft.end()) {
            int32 Completed = 0;
            for (const auto& Service : Match->services) if (Service.status == airside::TaskStatus::Completed) ++Completed;
            const int64 Delay = Match->actual_departure
                ? FMath::Max<int64>(0, (*Match->actual_departure - Match->scheduled_departure).count())
                : FMath::Max<int64>(0, (GetPlaybackTime() - Match->scheduled_departure).count());
            FString GateName(TEXT("Unknown gate"));
            const auto Gate = std::ranges::find(Value.gates, Match->assigned_gate, &airside::GateSnapshot::id);
            if (Gate != Value.gates.end()) GateName = UTF8_TO_TCHAR(Gate->name.c_str());
            return FString::Printf(TEXT("%s\nGate %s  /  %s\nScheduled departure %02lld:%02lld\nCurrent delay %.1f min\nServices %d / %d complete"),
                UTF8_TO_TCHAR(Match->flight_number.c_str()), *GateName, *AircraftStateText(Match->state),
                Match->scheduled_departure.count() / 3600, (Match->scheduled_departure.count() / 60) % 60,
                Delay / 60.0, Completed, static_cast<int32>(Match->services.size()));
        }
    } else if (SelectedEntityKind == TEXT("vehicle")) {
        const auto Match = std::ranges::find(Value.vehicles, SelectedEntityId, [](const auto& Item) { return Item.id.value(); });
        if (Match != Value.vehicles.end()) {
            FString Route(TEXT("Route: "));
            if (Match->journey && !Match->journey->route_nodes.empty()) {
                for (size_t Index = 0; Index < Match->journey->route_nodes.size(); ++Index) {
                    const auto Node = std::ranges::find(Value.road_nodes, Match->journey->route_nodes[Index], &airside::RoadNodeSnapshot::id);
                    if (Index > 0) Route += TEXT(" -> ");
                    Route += Node == Value.road_nodes.end() ? TEXT("?") : UTF8_TO_TCHAR(Node->name.c_str());
                }
            } else {
                Route += Match->fleet_status.empty()
                    ? TEXT("At depot") : UTF8_TO_TCHAR(Match->fleet_status.c_str());
            }
            FString Assignment(TEXT("Unassigned"));
            if (Match->assigned_aircraft) {
                const auto Aircraft = std::ranges::find(Value.aircraft, *Match->assigned_aircraft, &airside::AircraftSnapshot::id);
                if (Aircraft != Value.aircraft.end()) Assignment = UTF8_TO_TCHAR(Aircraft->flight_number.c_str());
            }
            return FString::Printf(TEXT("%s\n%s vehicle\n%s\nAssigned: %s\n%s"),
                UTF8_TO_TCHAR(Match->name.c_str()),
                Match->type == airside::ServiceType::Fueling ? TEXT("Fuel") : TEXT("Baggage"),
                Match->fleet_status.empty() ? *VehicleStateText(Match->state) : UTF8_TO_TCHAR(Match->fleet_status.c_str()),
                *Assignment, *Route);
        }
    } else if (SelectedEntityKind == TEXT("gate")) {
        const auto Match = std::ranges::find(Value.gates, SelectedEntityId, [](const auto& Item) { return Item.id.value(); });
        if (Match != Value.gates.end()) {
            FString Occupant(TEXT("None"));
            if (Match->occupying_aircraft) {
                const auto Aircraft = std::ranges::find(Value.aircraft, *Match->occupying_aircraft, &airside::AircraftSnapshot::id);
                if (Aircraft != Value.aircraft.end()) Occupant = UTF8_TO_TCHAR(Aircraft->flight_number.c_str());
            }
            return FString::Printf(TEXT("Gate %s\n%s\nAircraft: %s"), UTF8_TO_TCHAR(Match->name.c_str()),
                Match->available ? TEXT("Available") : TEXT("Occupied"), *Occupant);
        }
    }
    return TEXT("Selection unavailable");
}

bool URampLabSimulationSubsystem::IsRoadOnSelectedRoute(uint32 RoadId) const
{
    if (!Snapshot.IsSet() || SelectedEntityKind != TEXT("vehicle")) return false;
    const auto& Vehicles = Snapshot->vehicles;
    const auto Match = std::ranges::find(Vehicles, SelectedEntityId, [](const auto& Item) { return Item.id.value(); });
    if (Match == Vehicles.end() || !Match->journey) return false;
    return std::ranges::any_of(Match->journey->route_edges, [RoadId](const auto Id) { return Id.value() == RoadId; });
}

void URampLabSimulationSubsystem::ReconcileSnapshot()
{
    if (Simulation != nullptr) Snapshot = Simulation->snapshot();
}

void URampLabSimulationSubsystem::RunControlCheck(float DeltaTime)
{
    if (!bViewerReady || Simulation == nullptr) return;
    ControlCheckWallSeconds += DeltaTime;

    if (ControlCheckStage == 0) {
        for (const double Speed : {1.0, 5.0, 10.0, 20.0}) {
            SetPlaybackSpeed(Speed);
            bControlCheckPassed = bControlCheckPassed && PlaybackSpeed == Speed;
        }
        SelectScenario(TEXT("high_capacity"));
        bControlCheckPassed = bControlCheckPassed
            && ScenarioName == TEXT("high_capacity")
            && Snapshot.IsSet()
            && Snapshot->vehicles.size() == 4
            && ComparisonText.Contains(TEXT("BASELINE"))
            && ComparisonText.Contains(TEXT("HIGH CAPACITY"));
        SelectEntityKind(TEXT("aircraft"));
        bControlCheckPassed = bControlCheckPassed && !GetSelectedEntityText().Contains(TEXT("unavailable"), ESearchCase::IgnoreCase);
        SelectEntityKind(TEXT("gate"));
        bControlCheckPassed = bControlCheckPassed && !GetSelectedEntityText().Contains(TEXT("unavailable"), ESearchCase::IgnoreCase);
        SelectEntityKind(TEXT("vehicle"));
        bControlCheckPassed = bControlCheckPassed && !GetSelectedEntityText().Contains(TEXT("unavailable"), ESearchCase::IgnoreCase);
        SetCameraPreset(TEXT("Gate A2"));
        bControlCheckPassed = bControlCheckPassed && CameraPreset == TEXT("Gate A2") && !GeospatialStatus.IsEmpty();
        SelectScenario(TEXT("baseline"));
        bControlCheckPassed = bControlCheckPassed && ScenarioName == TEXT("baseline");
        ControlCheckPausedTime = PlaybackSeconds;
        TogglePlaying();
        bControlCheckPassed = bControlCheckPassed && !bPlaying;
        ControlCheckWallSeconds = 0.0;
        ControlCheckStage = 1;
        return;
    }

    if (ControlCheckStage == 1 && ControlCheckWallSeconds >= 0.5) {
        bControlCheckPassed = bControlCheckPassed && PlaybackSeconds == ControlCheckPausedTime;
        TogglePlaying();
        bControlCheckPassed = bControlCheckPassed && bPlaying;
        ControlCheckStage = 2;
        return;
    }

    if (ControlCheckStage == 2 && PlaybackSeconds > ControlCheckPausedTime) {
        SelectedScenarioKey = TEXT("baseline");
        ResetSimulation();
        bControlCheckPassed = bControlCheckPassed
            && PlaybackSeconds == 0.0
            && Seed == 42
            && bPlaying
            && (PlaybackSpeed == 1.0 || PlaybackSpeed == 5.0 || PlaybackSpeed == 10.0 || PlaybackSpeed == 20.0);
        UE_LOG(LogRampLab, Display, TEXT("RampLab control check: %s (Play/Pause, Reset, scenario selection, entity inspection, camera preset, comparison, operator speeds 1x/5x/10x/20x only)"),
            bControlCheckPassed ? TEXT("PASSED") : TEXT("FAILED"));
        bControlCheck = false;
        FPlatformMisc::RequestExit(false);
    }
}
