#pragma once

#include "CoreMinimal.h"
#include "CesiumIonRasterOverlay.h"
#include "GameFramework/Actor.h"
#include "RampLabAirportEnvironment.generated.h"

class ACesium3DTileset;
class ACesiumGeoreference;
class UCesiumIonServer;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class USceneComponent;
class UStaticMesh;
class UStaticMeshComponent;
class UTextRenderComponent;

UCLASS()
class RAMPLABINTEGRATION_API URampLabIonRasterOverlay final : public UCesiumIonRasterOverlay
{
    GENERATED_BODY()

public:
    void EnableOnScreenCredits() { ShowCreditsOnScreen = true; }
};

UCLASS()
class RAMPLABINTEGRATION_API ARampLabAirportEnvironment final : public AActor
{
    GENERATED_BODY()

public:
    ARampLabAirportEnvironment();
    virtual void BeginPlay() override;

    [[nodiscard]] bool IsCesiumConnected() const noexcept { return bCesiumConnected; }
    [[nodiscard]] FString GetCesiumStatus() const { return CesiumStatus; }

private:
    UFUNCTION()
    void HandleTerrainLoaded();
    void BuildGeographicContext();
    void BuildOperationalContext();
    void AddBox(const FString& Name, FVector LocationCm, FVector SizeMeters, float YawDegrees, UMaterialInstanceDynamic* Material);
    UMaterialInstanceDynamic* CreateMaterial(FLinearColor Color);

    UPROPERTY() TObjectPtr<USceneComponent> SceneRoot;
    UPROPERTY() TObjectPtr<UStaticMesh> CubeMesh;
    UPROPERTY() TObjectPtr<UMaterialInterface> BaseMaterial;
    UPROPERTY() TObjectPtr<ACesiumGeoreference> Georeference;
    UPROPERTY() TObjectPtr<UCesiumIonServer> IonServer;
    UPROPERTY() TObjectPtr<ACesium3DTileset> Terrain;
    UPROPERTY() TArray<TObjectPtr<UStaticMeshComponent>> StaticGeometry;
    UPROPERTY() TArray<TObjectPtr<UTextRenderComponent>> AirportMarkings;
    bool bCesiumConnected{false};
    FString CesiumStatus{TEXT("Not initialized")};
};
