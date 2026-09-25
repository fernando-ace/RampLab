#include "RampLabAirportEnvironment.h"

#include "RampLabAirportPlacement.h"
#include "RampLabIntegration.h"
#include "RampLabSimulationSubsystem.h"

#include "Cesium3DTileset.h"
#include "CesiumGeoreference.h"
#include "CesiumIonRasterOverlay.h"
#include "CesiumIonServer.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "HAL/PlatformMisc.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UObject/ConstructorHelpers.h"

namespace {

FString LoadCesiumToken()
{
    FString Token = FPlatformMisc::GetEnvironmentVariable(TEXT("RAMPLAB_CESIUM_ION_TOKEN"));
    if (!Token.IsEmpty()) return Token;

    TArray<FString> Lines;
    const FString LocalEnvironment = FPaths::Combine(FPaths::ProjectDir(), TEXT(".env.local"));
    if (!FFileHelper::LoadFileToStringArray(Lines, *LocalEnvironment)) return {};
    constexpr TCHAR Prefix[] = TEXT("RAMPLAB_CESIUM_ION_TOKEN=");
    for (FString Line : Lines) {
        Line.TrimStartAndEndInline();
        if (!Line.StartsWith(Prefix)) continue;
        Token = Line.RightChop(UE_ARRAY_COUNT(Prefix) - 1).TrimStartAndEnd();
        if (Token.StartsWith(TEXT("\"")) && Token.EndsWith(TEXT("\"")) && Token.Len() >= 2) {
            Token = Token.Mid(1, Token.Len() - 2);
        }
        return Token;
    }
    return {};
}

} // namespace

ARampLabAirportEnvironment::ARampLabAirportEnvironment()
{
    PrimaryActorTick.bCanEverTick = false;
    SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("AirportEnvironmentRoot"));
    RootComponent = SceneRoot;
    static ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(TEXT("/Engine/BasicShapes/Cube.Cube"));
    CubeMesh = Cube.Object;
    static ConstructorHelpers::FObjectFinder<UMaterialInterface> BasicMaterial(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
    BaseMaterial = BasicMaterial.Object;
}

void ARampLabAirportEnvironment::BeginPlay()
{
    Super::BeginPlay();
    BuildGeographicContext();
    BuildOperationalContext();
}

void ARampLabAirportEnvironment::BuildGeographicContext()
{
    const FRampLabAirportPlacement Placement = FRampLabAirportPlacement::Load();
    Georeference = GetWorld()->SpawnActor<ACesiumGeoreference>();
    Georeference->SetOriginLongitudeLatitudeHeight(FVector(Placement.Longitude, Placement.Latitude, Placement.HeightMeters));
    UE_LOG(LogRampLab, Display,
        TEXT("KAUO georeference: lat=%.7f lon=%.7f ellipsoid_height=%.2fm local_x_bearing=%.1fdeg offset=(%.1f, %.1f)m scale=%.2f"),
        Placement.Latitude, Placement.Longitude, Placement.HeightMeters, Placement.SimulationHeadingDegrees,
        Placement.OriginOffsetMeters.X, Placement.OriginOffsetMeters.Y, Placement.Scale);

    FString Token = LoadCesiumToken();
    if (Token.IsEmpty()) {
        CesiumStatus = TEXT("Georeferenced at KAUO; set RAMPLAB_CESIUM_ION_TOKEN for terrain and aerial imagery");
        if (auto* Subsystem = GetGameInstance()->GetSubsystem<URampLabSimulationSubsystem>()) Subsystem->SetGeospatialStatus(CesiumStatus);
        UE_LOG(LogRampLab, Warning, TEXT("%s"), *CesiumStatus);
        return;
    }

    // A transient server avoids the editor-only default-server asset creation
    // path during `-game` runs. It contains public endpoint configuration only;
    // the credential remains on the individual runtime components.
    IonServer = NewObject<UCesiumIonServer>(this, TEXT("RampLabCesiumIonServer"), RF_Transient);
    IonServer->DisplayName = TEXT("Cesium ion");
    IonServer->ServerUrl = TEXT("https://ion.cesium.com");
    IonServer->ApiUrl = TEXT("https://api.cesium.com");
    UCesiumIonServer::SetServerForNewObjects(IonServer);

    // Defer BeginPlay so Cesium never issues a transient request for its
    // default asset ID before RampLab applies the configured terrain asset.
    Terrain = GetWorld()->SpawnActorDeferred<ACesium3DTileset>(
        ACesium3DTileset::StaticClass(), FTransform::Identity);
    Terrain->SetCesiumIonServer(IonServer);
    Terrain->SetIonAssetID(Placement.TerrainAssetId);
    Terrain->SetIonAccessToken(Token);
    Terrain->SetMaximumScreenSpaceError(16.0);
    Terrain->MaximumCachedBytes = 256LL * 1024LL * 1024LL;
    Terrain->ShowCreditsOnScreen = true;
    Terrain->FinishSpawning(FTransform::Identity);

    auto* Imagery = NewObject<URampLabIonRasterOverlay>(Terrain, TEXT("AuburnAerialImagery"));
    Imagery->IonAssetID = Placement.ImageryAssetId;
    Imagery->IonAccessToken = Token;
    Imagery->CesiumIonServer = IonServer;
    Imagery->EnableOnScreenCredits();
    Terrain->AddInstanceComponent(Imagery);
    Imagery->RegisterComponent();
    Terrain->ResolveCreditSystem();

    Token.Reset();
    bCesiumConnected = true;
    CesiumStatus = TEXT("Cesium World Terrain and aerial imagery requested for KAUO");
    if (auto* Subsystem = GetGameInstance()->GetSubsystem<URampLabSimulationSubsystem>()) Subsystem->SetGeospatialStatus(CesiumStatus);
    UE_LOG(LogRampLab, Display, TEXT("%s (credits visible)"), *CesiumStatus);
}

void ARampLabAirportEnvironment::BuildOperationalContext()
{
    // The basic-shape material is lit in linear space. Keep the base values
    // deliberately restrained so automatic exposure does not wash the overlay
    // out against darker aerial imagery.
    auto* Runway = CreateMaterial(FLinearColor(0.0025f, 0.0035f, 0.0050f));
    auto* Taxiway = CreateMaterial(FLinearColor(0.006f, 0.008f, 0.010f));
    auto* Apron = CreateMaterial(FLinearColor(0.012f, 0.015f, 0.017f));
    auto* Marking = CreateMaterial(FLinearColor(0.32f, 0.21f, 0.015f));
    auto* Building = CreateMaterial(FLinearColor(0.018f, 0.024f, 0.030f));

    // FAA-published runway dimensions; surrounding ramp/taxiway/building blocks are a
    // deliberately synthetic operational layer aligned for the RampLab demonstration.
    constexpr float RunwayYaw = -84.3f;
    AddBox(TEXT("Runway18_36"), FVector(-5000.0, 0.0, 0.0), FVector(1604.7, 30.5, 0.35), RunwayYaw, Runway);
    AddBox(TEXT("RunwayCenterline"), FVector(-5000.0, 0.0, 25.0), FVector(1520.0, 0.7, 0.08), RunwayYaw, Marking);
    AddBox(TEXT("TaxiwayConnector"), FVector(10000.0, 0.0, 0.0), FVector(260.0, 18.0, 0.25), 0.0f, Taxiway);
    // The focused apron follows the three synthetic stands without covering
    // the surrounding real aerial context. It is intentionally not a claim
    // about Auburn's published ramp geometry.
    AddBox(TEXT("RampApron"), FVector(30000.0, 0.0, -10.0), FVector(115.0, 245.0, 0.25), 0.0f, Apron);
    AddBox(TEXT("Terminal"), FVector(39000.0, -13000.0, 700.0), FVector(92.0, 28.0, 14.0), RunwayYaw, Building);
    AddBox(TEXT("HangarEast"), FVector(44000.0, 7000.0, 550.0), FVector(48.0, 36.0, 11.0), RunwayYaw, Building);
    AddBox(TEXT("ServiceDepot"), FVector(8000.0, 0.0, 280.0), FVector(34.0, 24.0, 5.5), RunwayYaw, Building);
}

void ARampLabAirportEnvironment::AddBox(
    const FString& Name,
    FVector LocationCm,
    FVector SizeMeters,
    float YawDegrees,
    UMaterialInstanceDynamic* Material)
{
    const FRampLabAirportPlacement Placement = FRampLabAirportPlacement::Load();
    auto* Mesh = NewObject<UStaticMeshComponent>(this, *Name);
    Mesh->SetupAttachment(SceneRoot);
    Mesh->SetStaticMesh(CubeMesh);
    Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Mesh->SetWorldLocation(LocationCm + FVector(0.0, 0.0, Placement.OperationalLayerHeightCm));
    Mesh->SetWorldRotation(FRotator(0.0, YawDegrees, 0.0));
    Mesh->SetWorldScale3D(SizeMeters / 1.0);
    Mesh->SetMaterial(0, Material);
    Mesh->RegisterComponent();
    StaticGeometry.Add(Mesh);
}

UMaterialInstanceDynamic* ARampLabAirportEnvironment::CreateMaterial(FLinearColor Color)
{
    auto* Material = UMaterialInstanceDynamic::Create(
        BaseMaterial != nullptr ? BaseMaterial : UMaterial::GetDefaultMaterial(MD_Surface), this);
    Material->SetVectorParameterValue(TEXT("Color"), Color);
    Material->SetVectorParameterValue(TEXT("BaseColor"), Color);
    return Material;
}
