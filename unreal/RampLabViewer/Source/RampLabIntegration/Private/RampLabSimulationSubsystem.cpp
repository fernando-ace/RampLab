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
            bCompletionReported = true;
            FinalResultText = FormatResult(ScenarioName.ToUpper(), Result);
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
        constexpr int32 MaxEvents = 8;
        if (RecentEvents.Num() > MaxEvents) RecentEvents.RemoveAt(0, RecentEvents.Num() - MaxEvents);
        UE_LOG(LogRampLab, Log, TEXT("%s"), *Message);
    } catch (...) {
        UE_LOG(LogRampLab, Warning, TEXT("Failed to format simulation event %llu"), Event.sequence);
    }
}

void URampLabSimulationSubsystem::TogglePlaying()
{
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
        ScenarioKey != TEXT("autonomy_tug") && ScenarioKey != TEXT("autonomy_sensor_validation")) return;
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

bool URampLabSimulationSubsystem::LoadSelectedScenario()
{
    try {
        AutonomySimulation.Reset();
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
            RecentEvents.Reset(); PlaybackSeconds = 0.0; bPlaying = true; bCompletionReported = false;
            FinalResultText.Reset(); StatusText = FString::Printf(TEXT("Loaded A* Depot to Gate A2 autonomy mission with seed %llu"), Seed);
            UE_LOG(LogRampLab, Display, TEXT("%s"), *StatusText);
            return true;
        }
        auto Scenario = airside::load_scenario(std::filesystem::path{*Path});
        ScenarioName = UTF8_TO_TCHAR(Scenario.name.c_str());
        Seed = airside::resolve_seed(Scenario, std::nullopt);
        RecentEvents.Reset();
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
                Route += TEXT("At depot");
            }
            FString Assignment(TEXT("Unassigned"));
            if (Match->assigned_aircraft) {
                const auto Aircraft = std::ranges::find(Value.aircraft, *Match->assigned_aircraft, &airside::AircraftSnapshot::id);
                if (Aircraft != Value.aircraft.end()) Assignment = UTF8_TO_TCHAR(Aircraft->flight_number.c_str());
            }
            return FString::Printf(TEXT("%s\n%s vehicle\n%s\nAssigned: %s\n%s"),
                UTF8_TO_TCHAR(Match->name.c_str()),
                Match->type == airside::ServiceType::Fueling ? TEXT("Fuel") : TEXT("Baggage"),
                *VehicleStateText(Match->state), *Assignment, *Route);
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
