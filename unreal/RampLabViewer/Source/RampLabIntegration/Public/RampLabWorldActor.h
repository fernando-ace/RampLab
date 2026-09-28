#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "RampLabAirportPlacement.h"

#include "RampLabWorldActor.generated.h"

class UMaterialInstanceDynamic;
class UMaterialInterface;
class UCameraComponent;
class USceneComponent;
class UStaticMesh;
class UStaticMeshComponent;
class UTextRenderComponent;
class AStaticMeshActor;
struct FLinearColor;
namespace airside { struct SimulationSnapshot; struct Vec2; namespace autonomy { struct AutonomySnapshot; } }

UCLASS()
class RAMPLABINTEGRATION_API ARampLabWorldActor final : public AActor
{
    GENERATED_BODY()

public:
    ARampLabWorldActor();
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;
    virtual void CalcCamera(float DeltaTime, FMinimalViewInfo& OutResult) override;

private:
    void BuildTopology(const airside::SimulationSnapshot& Snapshot);
    void ClearTopology();
    void Reconcile(const airside::SimulationSnapshot& Snapshot, float DeltaSeconds);
    void BuildAutonomyTopology(const airside::autonomy::AutonomySnapshot& Snapshot);
    void ReconcileAutonomy(const airside::autonomy::AutonomySnapshot& Snapshot);
    void MaybeCapture();
    void UpdateCamera(float DeltaSeconds);
    void ApplyCameraPreset(const FString& Preset);
    UStaticMeshComponent* CreateMeshComponent(const FString& Prefix, uint32 Id, UStaticMesh* Mesh);
    AStaticMeshActor* CreateMirrorActor(const FString& Prefix, uint32 Id, UStaticMesh* Mesh);
    UTextRenderComponent* CreateLabel(const FString& Prefix, uint32 Id);
    UMaterialInstanceDynamic* CreateMaterial(const FLinearColor& Color);
    FVector ToWorld(airside::Vec2 Meters, float HeightCm = 0.0f) const;

    UPROPERTY() TObjectPtr<USceneComponent> SceneRoot;
    UPROPERTY() TObjectPtr<UCameraComponent> CameraComponent;
    UPROPERTY() TObjectPtr<UStaticMesh> CubeMesh;
    UPROPERTY() TObjectPtr<UStaticMesh> CylinderMesh;
    UPROPERTY() TObjectPtr<UMaterialInterface> BaseMaterial;
    UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> RoadOpenMaterial;
    UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> RoadClosedMaterial;
    UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> ActiveRouteMaterial;
    UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> GateOpenMaterial;
    UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> GateOccupiedMaterial;
    UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> AircraftMaterial;
    UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> AircraftWaitingMaterial;
    UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> AircraftReadyMaterial;
    UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> FuelMaterial;
    UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> BaggageMaterial;
    UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> AutonomyMaterial;
    UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> GnssMaterial;
    UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> ObstacleMaterial;
    UPROPERTY() TObjectPtr<AStaticMeshActor> AutonomyVehicleActor;
    UPROPERTY() TObjectPtr<AStaticMeshActor> GnssMarkerActor;
    UPROPERTY() TArray<TObjectPtr<AStaticMeshActor>> AutonomyObstacleActors;

    UPROPERTY() TMap<uint32, TObjectPtr<UStaticMeshComponent>> RoadMeshes;
    UPROPERTY() TMap<uint32, TObjectPtr<UStaticMeshComponent>> ClosureBarriers;
    UPROPERTY() TMap<uint32, TObjectPtr<AStaticMeshActor>> GateActors;
    UPROPERTY() TMap<uint32, TObjectPtr<AStaticMeshActor>> AircraftActors;
    UPROPERTY() TMap<uint32, TObjectPtr<AStaticMeshActor>> AircraftWingActors;
    UPROPERTY() TMap<uint32, TObjectPtr<AStaticMeshActor>> VehicleActors;
    UPROPERTY() TMap<uint32, TObjectPtr<AStaticMeshActor>> VehicleDetailActors;
    UPROPERTY() TMap<uint32, TObjectPtr<UTextRenderComponent>> GateLabels;
    UPROPERTY() TMap<uint32, TObjectPtr<UTextRenderComponent>> AircraftLabels;
    UPROPERTY() TMap<uint32, TObjectPtr<UTextRenderComponent>> VehicleLabels;
    bool bTopologyBuilt{false};
    uint32 TopologyGeneration{0};
    bool bCaptureRun{false};
    int32 CaptureStage{0};
    float CaptureExitCountdown{-1.0f};
    int32 DemoCameraStage{-1};
    FRampLabAirportPlacement Placement;
    FVector CameraFocus{FVector::ZeroVector};
    float CameraDistance{62000.0f};
    float CameraYaw{-35.0f};
    float CameraPitch{-52.0f};
    FString AppliedCameraPreset;
    FString BuiltScenario;
    double RuntimeWallSeconds{0.0};
    uint64 RuntimeFrames{0};
    bool bPerformanceReported{false};
    bool bAutonomyTopologyBuilt{false};
    double LastAutonomyTrailSampleTime{-1.0};
    TArray<FVector> AutonomyTrail;
};
