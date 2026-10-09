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
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "TimerManager.h"
#include "UnrealClient.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
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

    FString ScreenshotPath;
    FString ScreenshotCameraPreset = TEXT("KAUO Overview");
    FParse::Value(FCommandLine::Get(), TEXT("RampLabCameraPreset="), ScreenshotCameraPreset);
    if (FParse::Value(FCommandLine::Get(), TEXT("RampLabScreenshot="), ScreenshotPath) && !ScreenshotPath.IsEmpty()) {
        const FString ScreenshotDirectory = FPaths::GetPath(ScreenshotPath);
        if (!ScreenshotDirectory.IsEmpty()) IFileManager::Get().MakeDirectory(*ScreenshotDirectory, true);
        FTimerHandle ScreenshotTimer;
        GetWorld()->GetTimerManager().SetTimer(ScreenshotTimer, FTimerDelegate::CreateLambda([ScreenshotPath, ScreenshotCameraPreset]() {
            if (auto* World = GEngine == nullptr ? nullptr : GEngine->GetCurrentPlayWorld()) {
                if (auto* GameInstance = World->GetGameInstance()) {
                    if (auto* Subsystem = GameInstance->GetSubsystem<URampLabSimulationSubsystem>())
                        Subsystem->SetCameraPreset(ScreenshotCameraPreset);
                }
                FTimerHandle CaptureTimer;
                World->GetTimerManager().SetTimer(CaptureTimer, FTimerDelegate::CreateLambda([ScreenshotPath]() {
                    FScreenshotRequest::RequestScreenshot(ScreenshotPath, false, false);
                    UE_LOG(LogRampLab, Display, TEXT("Requested delayed KAUO validation screenshot: %s"), *ScreenshotPath);
                }), 2.0f, false);
            }
        }), 35.0f, false);
    }
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
    Terrain->SetGeoreference(Georeference);
    Terrain->SetCesiumIonServer(IonServer);
    Terrain->SetIonAssetID(Placement.TerrainAssetId);
    Terrain->SetIonAccessToken(Token);
    Terrain->SetMaximumScreenSpaceError(16.0);
    Terrain->MaximumCachedBytes = 256LL * 1024LL * 1024LL;
    Terrain->ShowCreditsOnScreen = true;
    Terrain->FinishSpawning(FTransform::Identity);

    const ACesiumGeoreference* ResolvedGeoreference = Terrain->ResolveGeoreference();
    const FVector ResolvedOrigin = ResolvedGeoreference == nullptr
        ? FVector::ZeroVector
        : ResolvedGeoreference->GetOriginLongitudeLatitudeHeight();
    UE_LOG(LogRampLab, Display,
        TEXT("Cesium tileset georeference: resolved=%s origin=(lon=%.7f lat=%.7f height=%.2fm) explicit_binding=true"),
        ResolvedGeoreference == Georeference ? TEXT("expected") : TEXT("unexpected"),
        ResolvedOrigin.X, ResolvedOrigin.Y, ResolvedOrigin.Z);

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
    auto* RunwayMaterial = CreateMaterial(FLinearColor(0.0025f, 0.0035f, 0.0050f));
    auto* Taxiway = CreateMaterial(FLinearColor(0.006f, 0.008f, 0.010f));
    auto* Apron = CreateMaterial(FLinearColor(0.012f, 0.015f, 0.017f));
    auto* Marking = CreateMaterial(FLinearColor(0.32f, 0.21f, 0.015f));
    auto* Building = CreateMaterial(FLinearColor(0.018f, 0.024f, 0.030f));

    FString ScenarioPath;
    FParse::Value(FCommandLine::Get(), TEXT("RampLabScenarioFile="), ScenarioPath);
    FString ScenarioText;
    TSharedPtr<FJsonObject> ScenarioRoot;
    if (!ScenarioPath.IsEmpty() && FFileHelper::LoadFileToString(ScenarioText, *ScenarioPath)) {
        const auto JsonReader = TJsonReaderFactory<>::Create(ScenarioText);
        FJsonSerializer::Deserialize(JsonReader, ScenarioRoot);
    }
    const auto Airport = ScenarioRoot.IsValid() ? ScenarioRoot->GetObjectField(TEXT("airport")) : nullptr;
    const auto Features = Airport.IsValid() ? Airport->GetObjectField(TEXT("features")) : nullptr;
    bool bKauoCalibration = false;
    if (Features.IsValid()) Features->TryGetBoolField(TEXT("kauo_calibrated"), bKauoCalibration);

    if (bKauoCalibration) {
        const bool bGeospatialWireframe = FParse::Param(FCommandLine::Get(), TEXT("RampLabGeospatialWireframe"));
        const FRampLabAirportPlacement Placement = FRampLabAirportPlacement::Load();
        const auto ToOverlay = [&Placement](double X, double Y) {
            return Placement.ToUnreal({X, Y}, -Placement.OperationalLayerHeightCm);
        };
        const auto AddSegment = [this, &ToOverlay](const FString& Name, double X1, double Y1, double X2, double Y2,
            double WidthMeters, UMaterialInstanceDynamic* Material) {
            const FVector A = ToOverlay(X1, Y1);
            const FVector B = ToOverlay(X2, Y2);
            const FVector Delta = B - A;
            const FVector Center = (A + B) * 0.5f;
            const float Yaw = FMath::RadiansToDegrees(FMath::Atan2(Delta.Y, Delta.X));
            AddBox(Name, Center, FVector(Delta.Size2D() / 100.0, WidthMeters, 0.35), Yaw, Material);
        };

        const auto Runways = Features->GetArrayField(TEXT("runways"));
        for (const auto& Value : Runways) {
            const auto RunwayInfo = Value->AsObject();
            if (!RunwayInfo.IsValid()) continue;
            const auto Center = RunwayInfo->GetObjectField(TEXT("center_local_m"));
            double CenterX = 0.0, CenterY = 0.0, Length = 0.0, Width = 0.0, Bearing = 0.0;
            if (!Center.IsValid() || !Center->TryGetNumberField(TEXT("x"), CenterX)
                || !Center->TryGetNumberField(TEXT("y"), CenterY)
                || !RunwayInfo->TryGetNumberField(TEXT("length_m"), Length)
                || !RunwayInfo->TryGetNumberField(TEXT("width_m"), Width)
                || !RunwayInfo->TryGetNumberField(TEXT("true_heading_degrees_from_first_threshold"), Bearing)) continue;

            // Convert the true bearing to a unit vector in the simulator's XY frame.
            const double LocalBearing = FMath::DegreesToRadians(Bearing - (Placement.SimulationHeadingDegrees - 90.0));
            const double AxisX = FMath::Sin(LocalBearing);
            const double AxisY = FMath::Cos(LocalBearing);
            const FString Identifier = RunwayInfo->GetStringField(TEXT("identifier"));
            const FString RunwayName = TEXT("KAUO_Runway_") + Identifier.Replace(TEXT("/"), TEXT("_"));
            const double HalfLength = Length * 0.5;
            if (bGeospatialWireframe) {
                const double HalfWidth = Width * 0.5;
                for (const double Side : {-1.0, 1.0}) {
                    const double OffsetX = -AxisY * HalfWidth * Side;
                    const double OffsetY = AxisX * HalfWidth * Side;
                    AddSegment(RunwayName + (Side < 0.0 ? TEXT("_EdgeA") : TEXT("_EdgeB")),
                        CenterX - AxisX * HalfLength + OffsetX, CenterY - AxisY * HalfLength + OffsetY,
                        CenterX + AxisX * HalfLength + OffsetX, CenterY + AxisY * HalfLength + OffsetY,
                        1.2, Marking);
                }
            } else {
                AddSegment(RunwayName, CenterX - AxisX * HalfLength, CenterY - AxisY * HalfLength,
                    CenterX + AxisX * HalfLength, CenterY + AxisY * HalfLength, Width, RunwayMaterial);
            }
            AddSegment(RunwayName + TEXT("_Centerline"), CenterX - AxisX * HalfLength * 0.95,
                CenterY - AxisY * HalfLength * 0.95, CenterX + AxisX * HalfLength * 0.95,
                CenterY + AxisY * HalfLength * 0.95, bGeospatialWireframe ? 0.6 : 0.4, Marking);
        }

        const auto ApronCenter = Features->GetObjectField(TEXT("apron_center_local_m"));
        const auto ApronSize = Features->GetObjectField(TEXT("apron_size_m"));
        if (ApronCenter.IsValid() && ApronSize.IsValid()) {
            double X = 0.0, Y = 0.0, Width = 0.0, Length = 0.0;
            if (ApronCenter->TryGetNumberField(TEXT("x"), X) && ApronCenter->TryGetNumberField(TEXT("y"), Y)
                && ApronSize->TryGetNumberField(TEXT("x"), Width) && ApronSize->TryGetNumberField(TEXT("y"), Length)) {
                if (bGeospatialWireframe) {
                    const double HalfWidth = Width * 0.5;
                    const double HalfLength = Length * 0.5;
                    AddSegment(TEXT("KAUO_TerminalFboApron_North"), X - HalfWidth, Y + HalfLength,
                        X + HalfWidth, Y + HalfLength, 1.2, Marking);
                    AddSegment(TEXT("KAUO_TerminalFboApron_South"), X - HalfWidth, Y - HalfLength,
                        X + HalfWidth, Y - HalfLength, 1.2, Marking);
                    AddSegment(TEXT("KAUO_TerminalFboApron_East"), X + HalfWidth, Y - HalfLength,
                        X + HalfWidth, Y + HalfLength, 1.2, Marking);
                    AddSegment(TEXT("KAUO_TerminalFboApron_West"), X - HalfWidth, Y - HalfLength,
                        X - HalfWidth, Y + HalfLength, 1.2, Marking);
                } else {
                    AddBox(TEXT("KAUO_TerminalFboApron"), ToOverlay(X, Y), FVector(Width, Length, 0.25),
                        Placement.SimulationHeadingDegrees - 90.0f, Apron);
                }
            }
        }

        const auto Buildings = Features->GetArrayField(TEXT("buildings"));
        for (int32 Index = 0; Index < Buildings.Num(); ++Index) {
            const auto BuildingInfo = Buildings[Index]->AsObject();
            if (!BuildingInfo.IsValid()) continue;
            const auto Center = BuildingInfo->GetObjectField(TEXT("center_local_m"));
            const auto Size = BuildingInfo->GetObjectField(TEXT("size_m"));
            double X = 0.0, Y = 0.0, Width = 0.0, Length = 0.0, Heading = 0.0;
            if (!Center.IsValid() || !Size.IsValid() || !Center->TryGetNumberField(TEXT("x"), X)
                || !Center->TryGetNumberField(TEXT("y"), Y) || !Size->TryGetNumberField(TEXT("x"), Width)
                || !Size->TryGetNumberField(TEXT("y"), Length)
                || !BuildingInfo->TryGetNumberField(TEXT("heading_degrees"), Heading)) continue;
            const FString Name = BuildingInfo->GetStringField(TEXT("name"));
            AddBox(TEXT("KAUO_Building_") + FString::FromInt(Index), ToOverlay(X, Y), FVector(Width, Length, 12.0),
                Heading - 90.0f, Building);
            UE_LOG(LogRampLab, Verbose, TEXT("KAUO approximate landmark: %s"), *Name);
        }

        const auto Nodes = Airport->GetArrayField(TEXT("nodes"));
        const auto Edges = Airport->GetArrayField(TEXT("edges"));
        TMap<FString, FVector2D> NodePositions;
        for (const auto& Value : Nodes) {
            const auto Node = Value->AsObject();
            if (!Node.IsValid()) continue;
            FString Id;
            double X = 0.0, Y = 0.0;
            if (Node->TryGetStringField(TEXT("id"), Id) && Node->TryGetNumberField(TEXT("x_m"), X)
                && Node->TryGetNumberField(TEXT("y_m"), Y)) NodePositions.Add(Id, FVector2D(X, Y));
        }
        for (const auto& Value : Edges) {
            const auto Edge = Value->AsObject();
            if (!Edge.IsValid()) continue;
            FString From, To, Id;
            if (!Edge->TryGetStringField(TEXT("from"), From) || !Edge->TryGetStringField(TEXT("to"), To)
                || !Edge->TryGetStringField(TEXT("id"), Id) || !NodePositions.Contains(From) || !NodePositions.Contains(To)) continue;
            const FVector2D A = NodePositions[From], B = NodePositions[To];
            AddSegment(TEXT("KAUO_Taxi_") + Id, A.X, A.Y, B.X, B.Y, 13.0, Taxiway);
        }
        UE_LOG(LogRampLab, Display, TEXT("KAUO airport overlay built from generated scenario geometry: runways=%d graph_edges=%d approximate_features=true"),
            Runways.Num(), Edges.Num());
        return;
    }

    // FAA-published runway dimensions; surrounding ramp/taxiway/building blocks are a
    // deliberately synthetic operational layer aligned for the RampLab demonstration.
    constexpr float RunwayYaw = -84.3f;
    AddBox(TEXT("Runway18_36"), FVector(-5000.0, 0.0, 0.0), FVector(1604.7, 30.5, 0.35), RunwayYaw, RunwayMaterial);
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
