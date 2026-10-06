#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Tickable.h"

#include "airside/operations/simulation.hpp"
#include "airside/autonomy/simulation.hpp"
#include "airside/autonomy/fleet.hpp"

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
    void SelectScenario(const FString& ScenarioKey);
    void SelectEntityKind(const FString& Kind);
    void SetCameraPreset(const FString& Preset) { CameraPreset = Preset; }
    void SetGeospatialStatus(const FString& Status) { GeospatialStatus = Status; }
    void SetPlaybackSpeed(double NewSpeed);
    void AttachControlPanel();

    [[nodiscard]] bool IsReady() const noexcept { return Simulation != nullptr || AutonomySimulation != nullptr || FleetSimulation != nullptr; }
    [[nodiscard]] bool IsPlaying() const noexcept { return bPlaying; }
    [[nodiscard]] bool IsFinished() const noexcept;
    [[nodiscard]] double GetPlaybackSpeed() const noexcept { return PlaybackSpeed; }
    [[nodiscard]] bool IsCaptureAccelerationActive() const noexcept { return CaptureMultiplier > 1.0; }
    [[nodiscard]] airside::SimTime GetPlaybackTime() const noexcept;
    [[nodiscard]] const airside::SimulationSnapshot* GetSnapshot() const noexcept;
    [[nodiscard]] const airside::autonomy::AutonomySnapshot* GetAutonomySnapshot() const noexcept;
    [[nodiscard]] const airside::autonomy::AutonomyScenario* GetAutonomyScenario() const noexcept;
    [[nodiscard]] bool IsAutonomyMode() const noexcept { return AutonomySimulation != nullptr || FleetSimulation != nullptr; }
    [[nodiscard]] bool IsFleetMode() const noexcept { return FleetSimulation != nullptr; }
    [[nodiscard]] const std::vector<airside::autonomy::FleetVehicleSnapshot>& GetFleetSnapshots() const noexcept { return FleetSnapshots; }
    [[nodiscard]] const airside::autonomy::FleetMetrics& GetFleetMetrics() const noexcept { return FleetMetricsSnapshot; }
    [[nodiscard]] const TArray<FString>& GetRecentEvents() const noexcept { return RecentEvents; }
    [[nodiscard]] uint64 GetSeed() const noexcept { return Seed; }
    [[nodiscard]] FString GetScenarioName() const { return ScenarioName; }
    [[nodiscard]] FString GetStatusText() const { return StatusText; }
    [[nodiscard]] FString GetSelectedEntityText() const;
    [[nodiscard]] FString GetComparisonText() const { return ComparisonText; }
    [[nodiscard]] FString GetFinalResultText() const { return FinalResultText; }
    [[nodiscard]] bool IsRoadOnSelectedRoute(uint32 RoadId) const;
    [[nodiscard]] bool IsDemoMode() const noexcept { return bDemoMode; }
    [[nodiscard]] FString GetCameraPreset() const { return CameraPreset; }
    [[nodiscard]] FString GetGeospatialStatus() const { return GeospatialStatus; }

private:
    bool LoadSelectedScenario();
    FString FindScenarioPath(const FString& Filename) const;
    void BuildScenarioComparison();
    void ReconcileSnapshot();
    void RunControlCheck(float DeltaTime);

    TUniquePtr<airside::Simulation> Simulation;
    TUniquePtr<airside::autonomy::AutonomySimulation> AutonomySimulation;
    TUniquePtr<airside::autonomy::FleetSimulation> FleetSimulation;
    std::vector<airside::autonomy::FleetVehicleSnapshot> FleetSnapshots;
    airside::autonomy::FleetMetrics FleetMetricsSnapshot;
    std::size_t FleetEventCount{0};
    std::size_t FleetDispatchEventCount{0};
    TUniquePtr<airside::autonomy::ReferenceController> AutonomyController;
    TOptional<airside::autonomy::AutonomySnapshot> AutonomySnapshot;
    TOptional<airside::SimulationSnapshot> Snapshot;
    TArray<FString> RecentEvents;
    TArray<bool> RecentEventIsRecoveryLifecycle;
    TSharedPtr<SWidget> ControlPanel;
    FString ScenarioName;
    FString SelectedScenarioKey{TEXT("baseline")};
    FString StatusText;
    FString ComparisonText;
    FString FinalResultText;
    FString SelectedEntityKind{TEXT("vehicle")};
    FString CameraPreset{TEXT("Overview")};
    FString GeospatialStatus{TEXT("Initializing Cesium georeference")};
    uint32 SelectedEntityId{0};
    double PlaybackSeconds{0.0};
    double PlaybackSpeed{10.0};
    double CaptureMultiplier{1.0};
    double CaptureWarmupRemaining{0.0};
    uint64 Seed{42};
    bool bPlaying{true};
    bool bViewerReady{false};
    bool bCompletionReported{false};
    bool bControlCheck{false};
    bool bCaptureQA{false};
    bool bFleetValidation{false};
    bool bGoal12ClosureValidation{false};
    bool bGoal12RecoveryValidation{false};
    bool bGoal13DispatchValidation{false};
    bool bGoal13ReassignmentValidation{false};
    bool bTurnaroundValidation{false};
    bool bGoal18Validation{false};
    bool bDemoMode{false};
    bool bDemoAdvancedToHighCapacity{false};
    bool bDemoAdvancedToAutonomy{false};
    double AutonomyAccumulator{0.0};
    bool bControlCheckPassed{true};
    int32 ControlCheckStage{0};
    double ControlCheckWallSeconds{0.0};
    double ControlCheckPausedTime{0.0};
    double DemoTransitionWallSeconds{0.0};
};
