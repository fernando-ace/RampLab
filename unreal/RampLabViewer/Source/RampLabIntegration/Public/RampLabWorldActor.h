#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

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
namespace airside { struct SimulationSnapshot; struct Vec2; }

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
    void Reconcile(const airside::SimulationSnapshot& Snapshot);
    void MaybeCapture();
    UStaticMeshComponent* CreateMeshComponent(const FString& Prefix, uint32 Id, UStaticMesh* Mesh);
    AStaticMeshActor* CreateMirrorActor(const FString& Prefix, uint32 Id, UStaticMesh* Mesh);
    UTextRenderComponent* CreateLabel(const FString& Prefix, uint32 Id);
    UMaterialInstanceDynamic* CreateMaterial(const FLinearColor& Color);
    FVector ToWorld(airside::Vec2 Meters, float HeightCm = 0.0f) const;

    UPROPERTY() TObjectPtr<USceneComponent> SceneRoot;
    UPROPERTY() TObjectPtr<UCameraComponent> CameraComponent;
    UPROPERTY() TObjectPtr<UStaticMesh> CubeMesh;
    UPROPERTY() TObjectPtr<UMaterialInterface> BaseMaterial;
    UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> RoadOpenMaterial;
    UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> RoadClosedMaterial;
    UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> GateOpenMaterial;
    UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> GateOccupiedMaterial;
    UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> AircraftMaterial;
    UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> AircraftReadyMaterial;
    UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> FuelMaterial;
    UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> BaggageMaterial;

    UPROPERTY() TMap<uint32, TObjectPtr<UStaticMeshComponent>> RoadMeshes;
    UPROPERTY() TMap<uint32, TObjectPtr<AStaticMeshActor>> GateActors;
    UPROPERTY() TMap<uint32, TObjectPtr<AStaticMeshActor>> AircraftActors;
    UPROPERTY() TMap<uint32, TObjectPtr<AStaticMeshActor>> VehicleActors;
    UPROPERTY() TMap<uint32, TObjectPtr<UTextRenderComponent>> GateLabels;
    UPROPERTY() TMap<uint32, TObjectPtr<UTextRenderComponent>> AircraftLabels;
    UPROPERTY() TMap<uint32, TObjectPtr<UTextRenderComponent>> VehicleLabels;
    bool bTopologyBuilt{false};
    bool bCaptureRun{false};
    int32 CaptureStage{0};
};
