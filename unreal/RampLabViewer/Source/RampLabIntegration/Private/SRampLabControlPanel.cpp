#include "SRampLabControlPanel.h"

#include "RampLabSimulationSubsystem.h"
#include "airside/agents/aircraft.hpp"
#include "airside/agents/service_vehicle.hpp"

#include "Styling/AppStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#include <algorithm>
#include <cmath>

void SRampLabControlPanel::Construct(const FArguments& Arguments)
{
    SimulationSubsystem = Arguments._Subsystem;

    ChildSlot
    [
        SNew(SBox)
        .WidthOverride(430.0f)
        .Padding(14.0f)
        [
            SNew(SBorder)
            .BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
            .Padding(18.0f)
            [
                SNew(SVerticalBox)
                + SVerticalBox::Slot().AutoHeight()
                [
                    SNew(STextBlock)
                    .Text(FText::FromString(TEXT("RampLab")))
                    .Font(FAppStyle::GetFontStyle("HeadingExtraSmall"))
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 12)
                [
                    SNew(STextBlock)
                    .Text(FText::FromString(TEXT("AUBURN UNIVERSITY REGIONAL AIRPORT  /  KAUO")))
                    .ColorAndOpacity(FLinearColor(0.45f, 0.72f, 0.88f))
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 12)
                [
                    SNew(STextBlock).Text(this, &SRampLabControlPanel::SummaryText)
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
                    [ SNew(SButton).Text(FText::FromString(TEXT("Play / Pause"))).OnClicked(this, &SRampLabControlPanel::TogglePlay) ]
                    + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 12, 0)
                    [ SNew(SButton).Text(FText::FromString(TEXT("Reset"))).OnClicked(this, &SRampLabControlPanel::Reset) ]
                    + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
                    [ SNew(SButton).Text(FText::FromString(TEXT("1x"))).OnClicked(this, &SRampLabControlPanel::SetSpeed, 1.0) ]
                    + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
                    [ SNew(SButton).Text(FText::FromString(TEXT("5x"))).OnClicked(this, &SRampLabControlPanel::SetSpeed, 5.0) ]
                    + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
                    [ SNew(SButton).Text(FText::FromString(TEXT("10x"))).OnClicked(this, &SRampLabControlPanel::SetSpeed, 10.0) ]
                    + SHorizontalBox::Slot().AutoWidth()
                    [ SNew(SButton).Text(FText::FromString(TEXT("20x"))).OnClicked(this, &SRampLabControlPanel::SetSpeed, 20.0) ]
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 10)
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
                    [ SNew(SButton).Text(FText::FromString(TEXT("Baseline"))).OnClicked(this, &SRampLabControlPanel::SelectScenario, FString(TEXT("baseline"))) ]
                    + SHorizontalBox::Slot().AutoWidth()
                    [ SNew(SButton).Text(FText::FromString(TEXT("High Capacity"))).OnClicked(this, &SRampLabControlPanel::SelectScenario, FString(TEXT("high_capacity"))) ]
                    + SHorizontalBox::Slot().AutoWidth().Padding(6, 0, 0, 0)
                    [ SNew(SButton).Text(FText::FromString(TEXT("Autonomy"))).OnClicked(this, &SRampLabControlPanel::SelectScenario, FString(TEXT("autonomy_tug"))) ]
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().AutoWidth()
                    [ SNew(SButton).Text(FText::FromString(TEXT("Sensor Validation"))).OnClicked(this, &SRampLabControlPanel::SelectScenario, FString(TEXT("autonomy_sensor_validation"))) ]
                    + SHorizontalBox::Slot().AutoWidth().Padding(0,0,6,0)
                    [ SNew(SButton).Text(FText::FromString(TEXT("Three Vehicle Fleet"))).OnClicked(this, &SRampLabControlPanel::SelectScenario, FString(TEXT("autonomy_fleet"))) ]
                    + SHorizontalBox::Slot().AutoWidth()
                    [ SNew(SButton).Text(FText::FromString(TEXT("Faulted Fleet"))).OnClicked(this, &SRampLabControlPanel::SelectScenario, FString(TEXT("autonomy_fleet_fault"))) ]
                    + SHorizontalBox::Slot().AutoWidth().Padding(6,0,0,0)
                    [ SNew(SButton).Text(FText::FromString(TEXT("Road Closure"))).OnClicked(this, &SRampLabControlPanel::SelectScenario, FString(TEXT("autonomy_fleet_dynamic_closure"))) ]
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
                [ SNew(SSeparator) ]
                + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 5)
                [ SNew(STextBlock).Text(FText::FromString(TEXT("SELECTED ENTITY"))).ColorAndOpacity(FLinearColor(0.75f, 0.78f, 0.80f)) ]
                + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 5)
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
                    [ SNew(SButton).Text(FText::FromString(TEXT("Next Aircraft"))).OnClicked(this, &SRampLabControlPanel::SelectEntity, FString(TEXT("aircraft"))) ]
                    + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
                    [ SNew(SButton).Text(FText::FromString(TEXT("Next Vehicle"))).OnClicked(this, &SRampLabControlPanel::SelectEntity, FString(TEXT("vehicle"))) ]
                    + SHorizontalBox::Slot().AutoWidth()
                    [ SNew(SButton).Text(FText::FromString(TEXT("Next Gate"))).OnClicked(this, &SRampLabControlPanel::SelectEntity, FString(TEXT("gate"))) ]
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 9)
                [ SNew(STextBlock).Text(this, &SRampLabControlPanel::SelectedEntityText).AutoWrapText(true) ]
                + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 5)
                [ SNew(STextBlock).Text(FText::FromString(TEXT("CAMERA"))).ColorAndOpacity(FLinearColor(0.75f, 0.78f, 0.80f)) ]
                + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 10)
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 5, 0)
                    [ SNew(SButton).Text(FText::FromString(TEXT("Overview"))).OnClicked(this, &SRampLabControlPanel::SetCamera, FString(TEXT("Overview"))) ]
                    + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 5, 0)
                    [ SNew(SButton).Text(FText::FromString(TEXT("Ramp"))).OnClicked(this, &SRampLabControlPanel::SetCamera, FString(TEXT("Ramp"))) ]
                    + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 5, 0)
                    [ SNew(SButton).Text(FText::FromString(TEXT("Gate A2"))).OnClicked(this, &SRampLabControlPanel::SetCamera, FString(TEXT("Gate A2"))) ]
                    + SHorizontalBox::Slot().AutoWidth()
                    [ SNew(SButton).Text(FText::FromString(TEXT("Roads"))).OnClicked(this, &SRampLabControlPanel::SetCamera, FString(TEXT("Service Roads"))) ]
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
                [ SNew(SSeparator) ]
                + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
                [ SNew(STextBlock).Text(FText::FromString(TEXT("SCENARIO RESULTS / AUTONOMY MISSION"))).ColorAndOpacity(FLinearColor(0.75f, 0.78f, 0.80f)) ]
                + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
                [ SNew(STextBlock).Text(this, &SRampLabControlPanel::ResultsText) ]
                + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
                [ SNew(STextBlock).Text(FText::FromString(TEXT("RECENT EVENTS"))).ColorAndOpacity(FLinearColor(0.75f, 0.78f, 0.80f)) ]
                + SVerticalBox::Slot().AutoHeight()
                [
                    SNew(STextBlock)
                    .Text(this, &SRampLabControlPanel::EventsText)
                    .AutoWrapText(true)
                ]
            ]
        ]
    ];
}

FText SRampLabControlPanel::SummaryText() const
{
    const auto* Subsystem = SimulationSubsystem.Get();
    if (Subsystem == nullptr || !Subsystem->IsReady()) {
        return FText::FromString(Subsystem == nullptr ? TEXT("Simulation unavailable") : Subsystem->GetStatusText());
    }

    if (Subsystem->IsFleetMode()) {
        const auto& Metrics=Subsystem->GetFleetMetrics();
        int32 Waiting=0,Stopped=0,Moving=0,Completed=0;
        FString VehicleLines,TaskLines;
        for(const auto& Vehicle:Subsystem->GetFleetSnapshots()){
            const TCHAR* State=Vehicle.waiting?TEXT("WAITING"):Vehicle.autonomy.finished?TEXT("COMPLETE"):Vehicle.autonomy.ground_truth.speed_mps<0.15?TEXT("STOPPED"):TEXT("MOVING");
            if(Vehicle.waiting)++Waiting;else if(Vehicle.autonomy.finished)++Completed;else if(Vehicle.autonomy.ground_truth.speed_mps<0.15)++Stopped;else ++Moving;
            VehicleLines+=FString::Printf(TEXT("\n%s -> %s  /  %s  / task %s  / recovery %s  %s  %.1f m  attempts %d"),
                UTF8_TO_TCHAR(Vehicle.id.value.c_str()),UTF8_TO_TCHAR(Vehicle.goal_node.c_str()),State,
                Vehicle.current_request?UTF8_TO_TCHAR(Vehicle.current_request->value.c_str()):TEXT("idle"),
                UTF8_TO_TCHAR(Vehicle.recovery_state.c_str()),UTF8_TO_TCHAR(Vehicle.recovery_resource.c_str()),
                Vehicle.retreat_progress_m,static_cast<int32>(Vehicle.recovery_attempts));
        }
        for(const auto& Task:Metrics.dispatch.requests){
            if(Task.state==airside::autonomy::ServiceTaskState::Completed)continue;
            TaskLines+=FString::Printf(TEXT("\n%s  %s  priority %lld  wait %.1f s  assigned %s"),
                UTF8_TO_TCHAR(Task.request.id.value.c_str()),UTF8_TO_TCHAR(airside::autonomy::to_string(Task.state).c_str()),
                static_cast<long long>(Task.effective_priority),Task.queue_wait_s,
                Task.assigned_vehicle?UTF8_TO_TCHAR(Task.assigned_vehicle->value.c_str()):TEXT("none"));
        }
        FString DependencyLine;
        if(!Metrics.wait_dependencies.empty()){
            const auto& Dependency=Metrics.wait_dependencies.front();
            DependencyLine=FString::Printf(TEXT("\nWaiting %s on %s via %s for %.2f s"),UTF8_TO_TCHAR(Dependency.waiting_vehicle.value.c_str()),UTF8_TO_TCHAR(Dependency.blocking_vehicle.value.c_str()),UTF8_TO_TCHAR(Dependency.resource.c_str()),Dependency.wait_duration_s);
        }
        const FString DispatchSummary=Metrics.dispatch.requests_created?FString::Printf(TEXT("\nDispatch %llu/%llu complete  /  %llu assigned  /  %llu reassigned  /  %llu aging  /  queue %.1f s avg, %.1f s max"),
            static_cast<unsigned long long>(Metrics.dispatch.requests_completed),static_cast<unsigned long long>(Metrics.dispatch.requests_created),
            static_cast<unsigned long long>(Metrics.dispatch.assignments),static_cast<unsigned long long>(Metrics.dispatch.reassignments),
            static_cast<unsigned long long>(Metrics.dispatch.aging_activations),Metrics.dispatch.average_queue_wait_s,Metrics.dispatch.maximum_queue_wait_s):FString();
        return FText::FromString(FString::Printf(TEXT("FLEET  %llu vehicles  /  seed %llu\nSimulation  %.2f s  /  %s\nMissions  %llu / %llu\nMoving %d   Waiting %d   Stopped %d   Complete %d\nTraffic wait %.2f s   Reservations %llu   Contention %llu\nNear conflicts %llu   Forced stops %llu   Deadlocks %llu  /  resolved %llu\nRecoveries %llu   Retreats %llu   Reroutes %llu   Closure replans %llu\nCollisions %llu   Minimum separation %.2f m%s%s%s%s"),
            static_cast<unsigned long long>(Metrics.vehicle_count),Subsystem->GetSeed(),static_cast<double>(Subsystem->GetPlaybackTime().count()),Subsystem->IsFinished()?TEXT("Finished"):Subsystem->IsPlaying()?TEXT("Running"):TEXT("Paused"),
            static_cast<unsigned long long>(Metrics.missions_completed),static_cast<unsigned long long>(Metrics.missions_attempted),Moving,Waiting,Stopped,Completed,Metrics.traffic_waiting_time_s,
            static_cast<unsigned long long>(Metrics.reservation_requests),static_cast<unsigned long long>(Metrics.reservation_contentions),static_cast<unsigned long long>(Metrics.near_conflict_events),static_cast<unsigned long long>(Metrics.forced_safety_stops),static_cast<unsigned long long>(Metrics.deadlock_count),static_cast<unsigned long long>(Metrics.deadlocks_resolved),static_cast<unsigned long long>(Metrics.recovery_attempts),static_cast<unsigned long long>(Metrics.retreat_count),static_cast<unsigned long long>(Metrics.reroutes),static_cast<unsigned long long>(Metrics.road_closure_replans),static_cast<unsigned long long>(Metrics.collisions),Metrics.minimum_separation_m,*DispatchSummary,*DependencyLine,*TaskLines,*VehicleLines));
    }

    if (Subsystem->IsAutonomyMode()) {
        const auto* State = Subsystem->GetAutonomySnapshot();
        if (State == nullptr) return FText::FromString(Subsystem->GetStatusText());
        double LidarMinimum = 0.0;
        if (State->sensors.lidar && !State->sensors.lidar->ranges_m.empty()) LidarMinimum = *std::min_element(State->sensors.lidar->ranges_m.begin(), State->sensors.lidar->ranges_m.end());
        double GoalDistance = 0.0;
        if (State->sensors.gnss) GoalDistance = std::hypot(State->mission.goal.x_m-State->sensors.gnss->position.x_m,State->mission.goal.y_m-State->sensors.gnss->position.y_m);
        const double EstimatedVelocity=State->sensors.odometry?State->sensors.odometry->speed_mps:0.0;
        return FText::FromString(FString::Printf(TEXT("Vehicle  Tug-1\nMission  Depot -> Gate A2\nController source  Built-in reference\nMission state  %s\nSimulation time  %.2f s\nSpeed  %.2f m/s    Estimated velocity  %.2f m/s\nEstimated distance to goal  %.1f m\nMinimum LiDAR range  %.2f m\nEmergency stops  %llu    Collisions  %llu\nGNSS estimate  %.1f, %.1f m\n%s"),
            State->finished ? TEXT("Finished") : (Subsystem->IsPlaying()?TEXT("Running"):TEXT("Paused")), State->timestamp_s,
            State->ground_truth.speed_mps, EstimatedVelocity, GoalDistance, LidarMinimum,
            static_cast<unsigned long long>(State->metrics.emergency_stops),static_cast<unsigned long long>(State->metrics.collision_count),
            State->sensors.gnss?State->sensors.gnss->position.x_m:0.0,State->sensors.gnss?State->sensors.gnss->position.y_m:0.0,
            *Subsystem->GetGeospatialStatus()));
    }
    const auto Time = Subsystem->GetPlaybackTime().count();
    const auto* Snapshot = Subsystem->GetSnapshot();
    int32 ActiveAircraft = 0;
    int32 DelayedAircraft = 0;
    int32 FuelTrucks = 0;
    int32 BaggageCarts = 0;
    bool bRoadClosure = false;
    if (Snapshot != nullptr) {
        for (const auto& Aircraft : Snapshot->aircraft) {
            if (Aircraft.state != airside::AircraftState::Scheduled && Aircraft.state != airside::AircraftState::Departed) ++ActiveAircraft;
            if (Aircraft.state != airside::AircraftState::Departed && Time > Aircraft.scheduled_departure.count()) ++DelayedAircraft;
        }
        for (const auto& Vehicle : Snapshot->vehicles) {
            Vehicle.type == airside::ServiceType::Fueling ? ++FuelTrucks : ++BaggageCarts;
        }
        bRoadClosure = std::ranges::any_of(Snapshot->roads, [](const auto& Road) { return !Road.enabled; });
    }

    return FText::FromString(FString::Printf(
        TEXT("Scenario  %s\nSimulation Time  %02lld:%02lld:%02lld\nPlayback  %.0fx  /  %s%s\n%s\n%s\nOPERATIONS\nAircraft Active  %d    Delayed  %d\nFuel Trucks  %d    Baggage Carts  %d"),
        *Subsystem->GetScenarioName(),
        Time / 3600, (Time / 60) % 60, Time % 60,
        Subsystem->GetPlaybackSpeed(),
        Subsystem->IsFinished() ? TEXT("Finished") : (Subsystem->IsPlaying() ? TEXT("Playing") : TEXT("Paused")),
        Subsystem->IsCaptureAccelerationActive() ? TEXT("   [QA capture acceleration]") : TEXT(""),
        *Subsystem->GetGeospatialStatus(),
        bRoadClosure ? TEXT("\nROAD CLOSURE  /  North to Gate A2 unavailable") : TEXT(""),
        ActiveAircraft, DelayedAircraft, FuelTrucks, BaggageCarts));
}

FText SRampLabControlPanel::EventsText() const
{
    const auto* Subsystem = SimulationSubsystem.Get();
    if (Subsystem == nullptr) return FText::GetEmpty();
    FString Result;
    const int32 Start = FMath::Max(0, Subsystem->GetRecentEvents().Num() - 8);
    for (int32 Index = Start; Index < Subsystem->GetRecentEvents().Num(); ++Index) {
        Result += Subsystem->GetRecentEvents()[Index] + TEXT("\n");
    }
    return FText::FromString(Result);
}

FText SRampLabControlPanel::SelectedEntityText() const
{
    const auto* Subsystem = SimulationSubsystem.Get();
    if (Subsystem != nullptr && Subsystem->IsFleetMode()) return FText::FromString(TEXT("Fleet labels show vehicle ID, goal, recovery state, retreat target, progress, and attempts. Recent Events preserves the retreat lifecycle alongside traffic and safety decisions."));
    if (Subsystem != nullptr && Subsystem->IsAutonomyMode()) return FText::FromString(TEXT("Tug-1 / ground truth body\nGNSS marker / sensor estimate\nBlue line / A* route\nYellow rays / Unreal geometry LiDAR"));
    return FText::FromString(Subsystem == nullptr ? TEXT("Unavailable") : Subsystem->GetSelectedEntityText());
}

FText SRampLabControlPanel::ResultsText() const
{
    const auto* Subsystem = SimulationSubsystem.Get();
    if (Subsystem != nullptr && Subsystem->IsAutonomyMode()) {
        if(Subsystem->IsFleetMode())return FText::FromString(Subsystem->IsFinished()?Subsystem->GetFinalResultText():TEXT("Fleet traffic metrics update during simulation. Recent Events shows typed coordination decisions."));
        const auto* State=Subsystem->GetAutonomySnapshot();
        return FText::FromString(State!=nullptr&&State->finished?Subsystem->GetFinalResultText():TEXT("Closed-loop mission is running from SensorFrame observations."));
    }
    return FText::FromString(Subsystem == nullptr ? TEXT("Unavailable") : Subsystem->GetComparisonText());
}

FReply SRampLabControlPanel::TogglePlay()
{
    if (auto* Subsystem = SimulationSubsystem.Get()) Subsystem->TogglePlaying();
    return FReply::Handled();
}

FReply SRampLabControlPanel::Reset()
{
    if (auto* Subsystem = SimulationSubsystem.Get()) Subsystem->ResetSimulation();
    return FReply::Handled();
}

FReply SRampLabControlPanel::SetSpeed(double Speed)
{
    if (auto* Subsystem = SimulationSubsystem.Get()) Subsystem->SetPlaybackSpeed(Speed);
    return FReply::Handled();
}

FReply SRampLabControlPanel::SelectScenario(FString Scenario)
{
    if (auto* Subsystem = SimulationSubsystem.Get()) Subsystem->SelectScenario(Scenario);
    return FReply::Handled();
}

FReply SRampLabControlPanel::SelectEntity(FString Kind)
{
    if (auto* Subsystem = SimulationSubsystem.Get()) Subsystem->SelectEntityKind(Kind);
    return FReply::Handled();
}

FReply SRampLabControlPanel::SetCamera(FString Preset)
{
    if (auto* Subsystem = SimulationSubsystem.Get()) Subsystem->SetCameraPreset(Preset);
    return FReply::Handled();
}
