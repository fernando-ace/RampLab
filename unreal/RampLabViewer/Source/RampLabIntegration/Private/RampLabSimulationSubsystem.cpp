#include "RampLabSimulationSubsystem.h"

#include "RampLabIntegration.h"
#include "SRampLabControlPanel.h"
#include "airside/core/event_stream.hpp"
#include "airside/scenario/scenario_loader.hpp"

#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "HAL/PlatformProcess.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Layout/SBox.h"

#include <algorithm>
#include <filesystem>

void URampLabSimulationSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    double RequestedSpeed = PlaybackSpeed;
    if (FParse::Value(FCommandLine::Get(), TEXT("RampLabPlaybackSpeed="), RequestedSpeed)) {
        PlaybackSpeed = FMath::Clamp(RequestedSpeed, 1.0, 100.0);
    }
    LoadBaseline();
}

void URampLabSimulationSubsystem::Deinitialize()
{
    if (ControlPanel.IsValid() && GEngine != nullptr && GEngine->GameViewport != nullptr) {
        GEngine->GameViewport->RemoveViewportWidgetContent(ControlPanel.ToSharedRef());
    }
    ControlPanel.Reset();
    Snapshot.Reset();
    Simulation.Reset();
    Super::Deinitialize();
}

void URampLabSimulationSubsystem::Tick(float DeltaTime)
{
    if (!bPlaying || Simulation == nullptr || Simulation->finished()) return;

    PlaybackSeconds += static_cast<double>(DeltaTime) * PlaybackSpeed;
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
    if (Simulation != nullptr && !Simulation->finished()) bPlaying = !bPlaying;
}

void URampLabSimulationSubsystem::ResetSimulation()
{
    LoadBaseline();
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
            .WidthOverride(410.0f)
            .HeightOverride(340.0f)
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

bool URampLabSimulationSubsystem::LoadBaseline()
{
    try {
        const FString Path = FindScenarioPath();
        if (Path.IsEmpty()) {
            StatusText = TEXT("baseline.yaml was not found");
            UE_LOG(LogRampLab, Error, TEXT("%s"), *StatusText);
            return false;
        }

        auto Scenario = airside::load_scenario(std::filesystem::path{*Path});
        ScenarioName = UTF8_TO_TCHAR(Scenario.name.c_str());
        Seed = airside::resolve_seed(Scenario, std::nullopt);
        RecentEvents.Reset();
        PlaybackSeconds = 0.0;
        bPlaying = true;
        bCompletionReported = false;
        Simulation = MakeUnique<airside::Simulation>(std::move(Scenario), Seed);
        Simulation->add_event_sink(*this);

        while (Simulation->next_event_time() && *Simulation->next_event_time() == airside::SimTime::zero()) {
            if (!Simulation->advance()) break;
        }
        ReconcileSnapshot();
        StatusText = FString::Printf(TEXT("Loaded %s with seed %llu"), *ScenarioName, Seed);
        UE_LOG(LogRampLab, Display, TEXT("%s from %s"), *StatusText, *Path);
        return true;
    } catch (const std::exception& Error) {
        Simulation.Reset();
        Snapshot.Reset();
        StatusText = FString::Printf(TEXT("RampLab initialization failed: %s"), UTF8_TO_TCHAR(Error.what()));
        UE_LOG(LogRampLab, Error, TEXT("%s"), *StatusText);
        return false;
    }
}

FString URampLabSimulationSubsystem::FindScenarioPath() const
{
    const TArray<FString> Candidates{
        FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT(".."), TEXT(".."), TEXT("scenarios"), TEXT("baseline.yaml"))),
        FPaths::ConvertRelativePathToFull(FPaths::Combine(FPlatformProcess::BaseDir(), TEXT("Scenarios"), TEXT("baseline.yaml"))),
    };
    for (const auto& Candidate : Candidates) {
        if (FPaths::FileExists(Candidate)) return Candidate;
    }
    return {};
}

void URampLabSimulationSubsystem::ReconcileSnapshot()
{
    if (Simulation != nullptr) Snapshot = Simulation->snapshot();
}
