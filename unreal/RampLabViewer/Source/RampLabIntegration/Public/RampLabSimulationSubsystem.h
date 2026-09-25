#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Tickable.h"

#include "airside/operations/simulation.hpp"

#include "RampLabSimulationSubsystem.generated.h"

class SWidget;

UCLASS()
class RAMPLABINTEGRATION_API URampLabSimulationSubsystem final
    : public UGameInstanceSubsystem
    , public FTickableGameObject
    , public airside::ISimulationEventSink
{
    GENERATED_BODY()

public:
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;
    virtual void Tick(float DeltaTime) override;
    virtual TStatId GetStatId() const override;
    virtual bool IsTickable() const override;
    virtual void on_event(const airside::SimulationEventRecord& Event) noexcept override;

    void TogglePlaying();
    void ResetSimulation();
    void SetPlaybackSpeed(double NewSpeed);
    void AttachControlPanel();

    [[nodiscard]] bool IsReady() const noexcept { return Simulation != nullptr; }
    [[nodiscard]] bool IsPlaying() const noexcept { return bPlaying; }
    [[nodiscard]] bool IsFinished() const noexcept;
    [[nodiscard]] double GetPlaybackSpeed() const noexcept { return PlaybackSpeed; }
    [[nodiscard]] bool IsCaptureAccelerationActive() const noexcept { return CaptureMultiplier > 1.0; }
    [[nodiscard]] airside::SimTime GetPlaybackTime() const noexcept;
    [[nodiscard]] const airside::SimulationSnapshot* GetSnapshot() const noexcept;
    [[nodiscard]] const TArray<FString>& GetRecentEvents() const noexcept { return RecentEvents; }
    [[nodiscard]] uint64 GetSeed() const noexcept { return Seed; }
    [[nodiscard]] FString GetScenarioName() const { return ScenarioName; }
    [[nodiscard]] FString GetStatusText() const { return StatusText; }

private:
    bool LoadBaseline();
    FString FindScenarioPath() const;
    void ReconcileSnapshot();
    void RunControlCheck(float DeltaTime);

    TUniquePtr<airside::Simulation> Simulation;
    TOptional<airside::SimulationSnapshot> Snapshot;
    TArray<FString> RecentEvents;
    TSharedPtr<SWidget> ControlPanel;
    FString ScenarioName;
    FString StatusText;
    double PlaybackSeconds{0.0};
    double PlaybackSpeed{10.0};
    double CaptureMultiplier{1.0};
    uint64 Seed{42};
    bool bPlaying{true};
    bool bViewerReady{false};
    bool bCompletionReported{false};
    bool bControlCheck{false};
    bool bControlCheckPassed{true};
    int32 ControlCheckStage{0};
    double ControlCheckWallSeconds{0.0};
    double ControlCheckPausedTime{0.0};
};
