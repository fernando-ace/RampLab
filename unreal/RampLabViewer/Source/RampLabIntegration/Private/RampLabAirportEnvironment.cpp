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
#include "Components/PointLightComponent.h"
#include "Components/TextRenderComponent.h"
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

#include <algorithm>

namespace {

struct FCesiumTokenResolution { FString Token; FString Source; };

FString ReadTokenFromFile(const FString& Path)
{
    TArray<FString> Lines;
    if (!FFileHelper::LoadFileToStringArray(Lines, *Path)) return {};
    for (FString Line : Lines) {
        Line.TrimStartAndEndInline();
        if (Line.StartsWith(TEXT("export "))) Line.RightChopInline(7);
        const int32 Separator = Line.Find(TEXT("="));
        if (Separator == INDEX_NONE || Line.Left(Separator).TrimStartAndEnd() != TEXT("RAMPLAB_CESIUM_ION_TOKEN")) continue;
        FString Value = Line.RightChop(Separator + 1).TrimStartAndEnd();
        if (Value.StartsWith(TEXT("\"")) || Value.StartsWith(TEXT("'"))) {
            const TCHAR Quote = Value[0];
            const int32 Closing = Value.Find(FString::Chr(Quote), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
            if (Closing > 0) Value = Value.Mid(1, Closing - 1);
        } else {
            const int32 Comment = Value.Find(TEXT("#"));
            if (Comment != INDEX_NONE) Value.LeftInline(Comment);
            Value.TrimStartAndEndInline();
        }
        if (Value.IsEmpty() || Value.Equals(TEXT("your_token_here"), ESearchCase::IgnoreCase)
            || Value.Equals(TEXT("<your-token>"), ESearchCase::IgnoreCase)) return {};
        return Value;
    }
    return {};
}

FString GetPrimaryCheckoutRoot(const FString& CurrentRoot)
{
    const FString GitMarker = FPaths::Combine(CurrentRoot, TEXT(".git"));
    if (IFileManager::Get().DirectoryExists(*GitMarker)) return CurrentRoot;
    FString Marker;
    if (!FFileHelper::LoadFileToString(Marker, *GitMarker)) return {};
    Marker.TrimStartAndEndInline();
    if (!Marker.StartsWith(TEXT("gitdir: "))) return {};
    FString GitDir = Marker.RightChop(8).TrimStartAndEnd();
    if (FPaths::IsRelative(GitDir)) GitDir = FPaths::Combine(CurrentRoot, GitDir);
    GitDir = FPaths::ConvertRelativePathToFull(GitDir);
    FString CommonDir;
    if (!FFileHelper::LoadFileToString(CommonDir, *FPaths::Combine(GitDir, TEXT("commondir")))) return {};
    CommonDir.TrimStartAndEndInline();
    if (FPaths::IsRelative(CommonDir)) CommonDir = FPaths::Combine(GitDir, CommonDir);
    return FPaths::GetPath(FPaths::ConvertRelativePathToFull(CommonDir));
}

FCesiumTokenResolution LoadCesiumToken()
{
    const FString ProjectRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT(".."), TEXT("..")));
    const FString PrimaryRoot = GetPrimaryCheckoutRoot(ProjectRoot);
    const TPair<FString, FString> RootCandidates[] = {
        {FPaths::Combine(ProjectRoot, TEXT(".local.env")), TEXT("current checkout local environment")},
        {PrimaryRoot.IsEmpty() ? FString() : FPaths::Combine(PrimaryRoot, TEXT(".local.env")), TEXT("primary checkout local environment")},
    };
    for (const auto& Candidate : RootCandidates) {
        FString Token = Candidate.Key.IsEmpty() ? FString() : ReadTokenFromFile(Candidate.Key);
        if (!Token.IsEmpty()) return {MoveTemp(Token), Candidate.Value};
    }
    FString Token = FPlatformMisc::GetEnvironmentVariable(TEXT("RAMPLAB_CESIUM_ION_TOKEN"));
    if (!Token.IsEmpty()) return {MoveTemp(Token), TEXT("process environment")};
    const TPair<FString, FString> SecureCandidates[] = {
        {FPaths::Combine(FPaths::ProjectDir(), TEXT(".env.local")), TEXT("current project local environment")},
        {FPaths::Combine(FPaths::ProjectConfigDir(), TEXT("CesiumIon.local.ini")), TEXT("current secure Cesium configuration")},
        {PrimaryRoot.IsEmpty() ? FString() : FPaths::Combine(PrimaryRoot, TEXT("unreal"), TEXT("RampLabViewer"), TEXT(".env.local")), TEXT("primary checkout local environment")},
        {PrimaryRoot.IsEmpty() ? FString() : FPaths::Combine(PrimaryRoot, TEXT("unreal"), TEXT("RampLabViewer"), TEXT("Config"), TEXT("CesiumIon.local.ini")), TEXT("primary secure Cesium configuration")},
    };
    for (const auto& Candidate : SecureCandidates) {
        FString CandidateToken = Candidate.Key.IsEmpty() ? FString() : ReadTokenFromFile(Candidate.Key);
        if (!CandidateToken.IsEmpty()) return {MoveTemp(CandidateToken), Candidate.Value};
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
    auto* Subsystem = GetGameInstance()->GetSubsystem<URampLabSimulationSubsystem>();
    const auto* Snapshot = Subsystem == nullptr ? nullptr : Subsystem->GetSnapshot();
    const bool bSyntheticDesign = Snapshot != nullptr && Snapshot->airport_design.has_value()
        && Snapshot->airport_design->name == "RampLab Demo Airport";
    if (bSyntheticDesign) {
        CesiumStatus = TEXT("Synthetic Demo Airport · local-meter coordinate frame · no geographic provider");
        if (Subsystem != nullptr) Subsystem->SetGeospatialStatus(CesiumStatus);
    } else {
        BuildGeographicContext();
    }
    BuildOperationalContext();

    FString ScreenshotPath;
    FString ScreenshotCameraPreset = TEXT("KAUO Overview");
    double ScreenshotDelaySeconds = 35.0;
    FParse::Value(FCommandLine::Get(), TEXT("RampLabCameraPreset="), ScreenshotCameraPreset);
    FParse::Value(FCommandLine::Get(), TEXT("RampLabScreenshotDelaySeconds="), ScreenshotDelaySeconds);
    if (!FMath::IsFinite(ScreenshotDelaySeconds) || ScreenshotDelaySeconds < 0.0) {
        UE_LOG(LogRampLab, Warning, TEXT("Ignoring invalid RampLabScreenshotDelaySeconds override: %.3f"), ScreenshotDelaySeconds);
        ScreenshotDelaySeconds = 35.0;
    }
    if (FParse::Value(FCommandLine::Get(), TEXT("RampLabScreenshot="), ScreenshotPath) && !ScreenshotPath.IsEmpty()) {
        const FString ScreenshotDirectory = FPaths::GetPath(ScreenshotPath);
        if (!ScreenshotDirectory.IsEmpty()) IFileManager::Get().MakeDirectory(*ScreenshotDirectory, true);
        FTimerHandle ScreenshotTimer;
        GetWorld()->GetTimerManager().SetTimer(ScreenshotTimer, FTimerDelegate::CreateLambda([ScreenshotPath, ScreenshotCameraPreset]() {
            if (auto* World = GEngine == nullptr ? nullptr : GEngine->GetCurrentPlayWorld()) {
                if (auto* GameInstance = World->GetGameInstance()) {
                    if (auto* Subsystem = GameInstance->GetSubsystem<URampLabSimulationSubsystem>()) {
                        Subsystem->SetCameraPreset(ScreenshotCameraPreset);
                        if (const auto* Snapshot = Subsystem->GetSnapshot()) {
                            FString AircraftStates;
                            for (const auto& Aircraft : Snapshot->aircraft) {
                                if (!AircraftStates.IsEmpty()) AircraftStates += TEXT(",");
                                AircraftStates += FString::Printf(TEXT("%s:%d@G%u"),
                                    UTF8_TO_TCHAR(Aircraft.flight_number.c_str()), static_cast<int32>(Aircraft.state),
                                    Aircraft.assigned_gate.value());
                            }
                            UE_LOG(LogRampLab, Display, TEXT("Goal 51 screenshot state: simulation_seconds=%lld aircraft=[%s]"),
                                Subsystem->GetPlaybackTime().count(), *AircraftStates);
                        }
                    }
                }
                FTimerHandle CaptureTimer;
                World->GetTimerManager().SetTimer(CaptureTimer, FTimerDelegate::CreateLambda([ScreenshotPath]() {
                    FScreenshotRequest::RequestScreenshot(ScreenshotPath, false, false);
                    UE_LOG(LogRampLab, Display, TEXT("Requested delayed KAUO validation screenshot: %s"), *ScreenshotPath);
                }), 2.0f, false);
            }
        }), static_cast<float>(ScreenshotDelaySeconds), false);
    }
}

void ARampLabAirportEnvironment::HandleTerrainLoaded()
{
    bCesiumConnected = true;
    CesiumStatus = TEXT("Cesium terrain connected; aerial imagery overlay configured for KAUO");
    if (auto* Subsystem = GetGameInstance()->GetSubsystem<URampLabSimulationSubsystem>())
        Subsystem->SetGeospatialStatus(CesiumStatus);
    UE_LOG(LogRampLab, Display, TEXT("Cesium terrain: connected; aerial imagery: configured; georeference: KAUO"));
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

    FCesiumTokenResolution TokenResolution = LoadCesiumToken();
    if (TokenResolution.Token.IsEmpty()) {
        CesiumStatus = TEXT("Georeferenced at KAUO; set RAMPLAB_CESIUM_ION_TOKEN for terrain and aerial imagery");
        if (auto* Subsystem = GetGameInstance()->GetSubsystem<URampLabSimulationSubsystem>()) Subsystem->SetGeospatialStatus(CesiumStatus);
        UE_LOG(LogRampLab, Warning, TEXT("%s"), *CesiumStatus);
        return;
    }
    UE_LOG(LogRampLab, Display, TEXT("Cesium ion token: loaded; source: %s"), *TokenResolution.Source);

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
    Terrain->SetIonAccessToken(TokenResolution.Token);
    Terrain->SetMaximumScreenSpaceError(16.0);
    Terrain->MaximumCachedBytes = 256LL * 1024LL * 1024LL;
    Terrain->ShowCreditsOnScreen = true;
    Terrain->OnTilesetLoaded.AddDynamic(this, &ARampLabAirportEnvironment::HandleTerrainLoaded);
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
    Imagery->IonAccessToken = TokenResolution.Token;
    Imagery->CesiumIonServer = IonServer;
    Imagery->EnableOnScreenCredits();
    Terrain->AddInstanceComponent(Imagery);
    Imagery->RegisterComponent();
    Terrain->ResolveCreditSystem();

    TokenResolution.Token.Reset();
    bCesiumConnected = false;
    CesiumStatus = TEXT("Cesium token loaded; terrain and aerial imagery configured for KAUO and awaiting streamed tiles");
    if (auto* Subsystem = GetGameInstance()->GetSubsystem<URampLabSimulationSubsystem>()) Subsystem->SetGeospatialStatus(CesiumStatus);
    UE_LOG(LogRampLab, Display, TEXT("%s (credits visible)"), *CesiumStatus);
}

void ARampLabAirportEnvironment::BuildOperationalContext()
{
    // The basic-shape material is lit in linear space. Keep the base values
    // deliberately restrained so automatic exposure does not wash the overlay
    // out against darker aerial imagery.
    auto* RunwayMaterial = CreateMaterial(FLinearColor(0.055f, 0.062f, 0.067f));
    auto* Taxiway = CreateMaterial(FLinearColor(0.075f, 0.082f, 0.078f));
    auto* Apron = CreateMaterial(FLinearColor(0.12f, 0.13f, 0.13f));
    auto* Marking = CreateMaterial(FLinearColor(0.84f, 0.84f, 0.78f));
    auto* TaxiMarking = CreateMaterial(FLinearColor(0.92f, 0.62f, 0.08f));
    auto* Building = CreateMaterial(FLinearColor(0.32f, 0.35f, 0.36f));
    auto* BuildingRoof = CreateMaterial(FLinearColor(0.19f, 0.22f, 0.23f));

    if (const auto* GameInstance = GetWorld()->GetGameInstance()) {
        if (const auto* Subsystem = GameInstance->GetSubsystem<URampLabSimulationSubsystem>()) {
            if (const auto* Snapshot = Subsystem->GetSnapshot(); Snapshot != nullptr
                && Snapshot->airport_design.has_value() && Snapshot->airport_design->name == "RampLab Demo Airport") {
                const auto& Design = *Snapshot->airport_design;
                const auto ToLocal = [](airside::Point2d Point, double Z = 0.0) {
                    return FVector(Point.x_m * 100.0, -Point.y_m * 100.0, Z);
                };
                auto AddLocalSegment = [this, &ToLocal](const FString& Name, airside::Point2d A, airside::Point2d B,
                    double WidthMeters, UMaterialInstanceDynamic* Material, double HeightCm = 24.0) {
                    const FVector Start = ToLocal(A, HeightCm), End = ToLocal(B, HeightCm);
                    const FVector Delta = End - Start;
                    AddBox(Name, (Start + End) * 0.5f, FVector(Delta.Size2D() / 100.0, WidthMeters, 0.12),
                        FMath::RadiansToDegrees(FMath::Atan2(Delta.Y, Delta.X)), Material);
                };
                auto AddFootprint = [this](const FString& Name, const std::vector<airside::Point2d>& Points,
                    UMaterialInstanceDynamic* Material, double ZCm = 12.0) -> FVector2D {
                    if (Points.empty()) return FVector2D::ZeroVector;
                    double MinX = Points.front().x_m, MaxX = MinX, MinY = Points.front().y_m, MaxY = MinY;
                    for (const auto& Point : Points) {
                        MinX = FMath::Min(MinX, Point.x_m); MaxX = FMath::Max(MaxX, Point.x_m);
                        MinY = FMath::Min(MinY, Point.y_m); MaxY = FMath::Max(MaxY, Point.y_m);
                    }
                    const FVector Center((MinX + MaxX) * 50.0, -(MinY + MaxY) * 50.0, ZCm);
                    AddBox(Name, Center, FVector(MaxX - MinX, MaxY - MinY, 0.24), 0.0f, Material);
                    return FVector2D((MinX + MaxX) * 0.5, (MinY + MaxY) * 0.5);
                };

                AddBox(TEXT("DemoAirport_Turf"), FVector(0, 0, -115), FVector(2800, 1900, 2), 0,
                    CreateMaterial(FLinearColor(0.105f, 0.19f, 0.095f)));
                AddBox(TEXT("DemoAirport_SouthApronVerge"), FVector(0, 37000, -2), FVector(1200, 70, 0.5), 0,
                    CreateMaterial(FLinearColor(0.20f, 0.28f, 0.14f)));
                auto* ShoulderMaterial = CreateMaterial(FLinearColor(0.105f, 0.112f, 0.116f));
                for (const auto& Runway : Design.runways) {
                    const FVector A = ToLocal(Runway.threshold_09), B = ToLocal(Runway.threshold_27);
                    const FVector Delta = B - A;
                    AddBox(FString(TEXT("DemoRunwayShoulders_")) + UTF8_TO_TCHAR(Runway.id.c_str()), (A + B) * 0.5f,
                        FVector(Delta.Size2D() / 100.0, Runway.width_m + 12.0, 0.36),
                        FMath::RadiansToDegrees(FMath::Atan2(Delta.Y, Delta.X)), ShoulderMaterial);
                }
                for (const auto& Region : Design.regions) {
                    UMaterialInstanceDynamic* Surface = Region.kind == "runway" ? RunwayMaterial
                        : Region.kind == "apron" ? Apron
                        : Region.kind == "service_road" ? CreateMaterial(FLinearColor(0.085f, 0.095f, 0.095f)) : Taxiway;
                    AddFootprint(FString(TEXT("DemoSurface_")) + UTF8_TO_TCHAR(Region.id.c_str()), Region.footprint, Surface, 8.0);
                }

                auto IsOnDesignSurface = [&Design](airside::Point2d Point) {
                    for (const auto& Region : Design.regions) {
                        if (Region.footprint.size() < 3) continue;
                        bool bInside = false;
                        for (std::size_t Current = 0, Previous = Region.footprint.size() - 1;
                            Current < Region.footprint.size(); Previous = Current++) {
                            const auto& A = Region.footprint[Previous];
                            const auto& B = Region.footprint[Current];
                            const bool bCrosses = ((A.y_m > Point.y_m) != (B.y_m > Point.y_m))
                                && (Point.x_m < (B.x_m - A.x_m) * (Point.y_m - A.y_m)
                                    / (B.y_m - A.y_m) + A.x_m);
                            if (bCrosses) bInside = !bInside;
                        }
                        if (bInside) return true;
                    }
                    return false;
                };

                TSet<FString> MarkedConnections;
                for (const auto& Road : Snapshot->roads) {
                    const uint32 Low = FMath::Min(Road.source.value(), Road.destination.value());
                    const uint32 High = FMath::Max(Road.source.value(), Road.destination.value());
                    const FString Key = FString::Printf(TEXT("%u-%u"), Low, High);
                    if (MarkedConnections.Contains(Key)) continue;
                    MarkedConnections.Add(Key);
                    std::vector<airside::Point2d> Points = Road.centerline;
                    if (Points.size() < 2) {
                        const auto From = std::ranges::find(Snapshot->road_nodes, Road.source, &airside::RoadNodeSnapshot::id);
                        const auto To = std::ranges::find(Snapshot->road_nodes, Road.destination, &airside::RoadNodeSnapshot::id);
                        if (From == Snapshot->road_nodes.end() || To == Snapshot->road_nodes.end()) continue;
                        Points = {From->position_m, To->position_m};
                    }
                    UMaterialInstanceDynamic* RoadSurface = Road.surface_type.find("taxiway") != std::string::npos
                        || Road.surface_type.find("asphalt") != std::string::npos ? Taxiway
                        : CreateMaterial(FLinearColor(0.095f, 0.104f, 0.101f));
                    for (std::size_t Index = 1; Index < Points.size(); ++Index) {
                        const auto Start = Points[Index - 1], End = Points[Index];
                        const double DX = End.x_m - Start.x_m, DY = End.y_m - Start.y_m;
                        const double Length = FMath::Sqrt(DX * DX + DY * DY);
                        if (Length < 0.1 || Road.width_m <= 0.0) continue;
                        const int32 Pieces = FMath::Max(1, FMath::CeilToInt(Length / 12.0));
                        for (int32 Piece = 0; Piece < Pieces; ++Piece) {
                            const double From = static_cast<double>(Piece) / Pieces;
                            const double To = static_cast<double>(Piece + 1) / Pieces;
                            const airside::Point2d A{Start.x_m + DX * From, Start.y_m + DY * From};
                            const airside::Point2d B{Start.x_m + DX * To, Start.y_m + DY * To};
                            const airside::Point2d Mid{(A.x_m + B.x_m) * 0.5, (A.y_m + B.y_m) * 0.5};
                            if (!IsOnDesignSurface(Mid))
                                AddLocalSegment(TEXT("DemoGraphDerivedPavement"), A, B, Road.width_m, RoadSurface, 12.0);
                        }
                    }
                    UMaterialInstanceDynamic* Centerline = Road.aircraft_allowed ? TaxiMarking : Marking;
                    for (std::size_t Index = 1; Index < Points.size(); ++Index) {
                        const auto Start = Points[Index - 1], End = Points[Index];
                        const double Length = FMath::Sqrt(FMath::Square(End.x_m - Start.x_m) + FMath::Square(End.y_m - Start.y_m));
                        if (Length < 0.1) continue;
                        if (Road.aircraft_allowed || Road.crosses_runway) {
                            AddLocalSegment(TEXT("DemoTaxiCenterline"), Start, End, 0.28, Centerline, 27.0);
                        } else {
                            const double DX = (End.x_m - Start.x_m) / Length, DY = (End.y_m - Start.y_m) / Length;
                            for (double Along = 0.0; Along < Length; Along += 24.0) {
                                const double DashEnd = FMath::Min(Along + 11.0, Length);
                                AddLocalSegment(TEXT("DemoServiceRoadDash"), {Start.x_m + DX * Along, Start.y_m + DY * Along},
                                    {Start.x_m + DX * DashEnd, Start.y_m + DY * DashEnd}, 0.20, Centerline, 26.0);
                            }
                        }
                    }
                }

                for (const auto& Runway : Design.runways) {
                    const double HalfLength = Runway.length_m * 0.5;
                    const double HalfWidth = Runway.width_m * 0.5;
                    const double CenterX = (Runway.threshold_09.x_m + Runway.threshold_27.x_m) * 0.5;
                    const double CenterY = (Runway.threshold_09.y_m + Runway.threshold_27.y_m) * 0.5;
                    AddLocalSegment(TEXT("DemoRunwayEdgeNorth"), {CenterX - HalfLength, CenterY + HalfWidth - 1.0},
                        {CenterX + HalfLength, CenterY + HalfWidth - 1.0}, 0.8, Marking, 34.0);
                    AddLocalSegment(TEXT("DemoRunwayEdgeSouth"), {CenterX - HalfLength, CenterY - HalfWidth + 1.0},
                        {CenterX + HalfLength, CenterY - HalfWidth + 1.0}, 0.8, Marking, 34.0);
                    for (double X = -HalfLength + 40.0; X < HalfLength - 24.0; X += 60.0)
                        AddLocalSegment(TEXT("DemoRunwayCenterline"), {CenterX + X, CenterY},
                            {CenterX + FMath::Min(X + 30.0, HalfLength - 20.0), CenterY}, 0.45, Marking, 35.0);
                    for (const double End : {-1.0, 1.0}) {
                        const double ThresholdX = CenterX + End * (HalfLength - 9.0);
                        for (int32 Stripe = 0; Stripe < 8; ++Stripe) {
                            const double Y = CenterY + (Stripe - 3.5) * (Runway.width_m - 6.0) / 8.0;
                            AddLocalSegment(TEXT("DemoRunwayThreshold"), {ThresholdX, Y}, {ThresholdX + End * 1.5, Y},
                                1.0, Marking, 35.0);
                        }
                    }
                    FString FirstDesignation, SecondDesignation;
                    if (Runway.designation.find('/') != std::string::npos) {
                        const auto Split = Runway.designation.find('/');
                        FirstDesignation = UTF8_TO_TCHAR(Runway.designation.substr(0, Split).c_str());
                        SecondDesignation = UTF8_TO_TCHAR(Runway.designation.substr(Split + 1).c_str());
                    }
                    const TPair<FString, double> Designations[] = {{FirstDesignation, -1.0}, {SecondDesignation, 1.0}};
                    for (int32 EndIndex = 0; EndIndex < 2; ++EndIndex) {
                        const auto& Designation = Designations[EndIndex];
                        if (Designation.Key.IsEmpty()) continue;
                        const FVector DesignationLocation = ToLocal({CenterX + Designation.Value * (HalfLength - 56.0), CenterY}, 39.0);
                        auto* Number = NewObject<UTextRenderComponent>(this, *FString::Printf(TEXT("DemoRunwayDesignation_%d"), EndIndex));
                        Number->SetupAttachment(SceneRoot);
                        Number->SetText(FText::FromString(Designation.Key));
                        Number->SetHorizontalAlignment(EHorizTextAligment::EHTA_Center);
                        Number->SetVerticalAlignment(EVerticalTextAligment::EVRTA_TextCenter);
                        Number->SetWorldSize(1350.0f);
                        Number->SetTextRenderColor(FColor(238, 237, 222));
                        Number->SetWorldLocation(DesignationLocation);
                        Number->SetWorldRotation(FRotator::ZeroRotator);
                        Number->SetCollisionEnabled(ECollisionEnabled::NoCollision);
                        Number->RegisterComponent();
                        AirportMarkings.Add(Number);
                    }
                }

                for (const auto& Hold : Design.hold_points) {
                    const auto Node = std::ranges::find(Snapshot->road_nodes, Hold.node, &airside::RoadNodeSnapshot::id);
                    if (Node == Snapshot->road_nodes.end()) continue;
                    for (const auto& Road : Snapshot->roads) {
                        if (Road.source != Hold.node && Road.destination != Hold.node) continue;
                        const auto OtherId = Road.source == Hold.node ? Road.destination : Road.source;
                        const auto Other = std::ranges::find(Snapshot->road_nodes, OtherId, &airside::RoadNodeSnapshot::id);
                        if (Other == Snapshot->road_nodes.end()) continue;
                        double DX = Other->position_m.x_m - Node->position_m.x_m;
                        double DY = Other->position_m.y_m - Node->position_m.y_m;
                        const double Length = FMath::Sqrt(DX * DX + DY * DY);
                        if (Length < 0.1) continue;
                        DX /= Length; DY /= Length;
                        const double HalfClearance = Hold.protected_clearance_m * 0.5;
                        const double AcrossX = -DY * HalfClearance, AcrossY = DX * HalfClearance;
                        for (int32 Bar = 0; Bar < 4; ++Bar) {
                            const double Along = Bar * 0.65;
                            AddLocalSegment(TEXT("DemoHoldShort"),
                                {Node->position_m.x_m + DX * Along - AcrossX, Node->position_m.y_m + DY * Along - AcrossY},
                                {Node->position_m.x_m + DX * Along + AcrossX, Node->position_m.y_m + DY * Along + AcrossY},
                                0.45, TaxiMarking, 30.0);
                        }
                        break;
                    }
                }
                for (const auto& Stand : Design.stands) {
                    for (std::size_t Index = 0; Index < Stand.footprint.size(); ++Index)
                        AddLocalSegment(FString(TEXT("DemoStandOutline_")) + UTF8_TO_TCHAR(Stand.id.c_str()), Stand.footprint[Index],
                            Stand.footprint[(Index + 1) % Stand.footprint.size()], 0.45, Marking, 28.0);
                    if (Stand.footprint.empty()) continue;
                    airside::Point2d Center{};
                    for (const auto& Point : Stand.footprint) { Center.x_m += Point.x_m; Center.y_m += Point.y_m; }
                    Center.x_m /= static_cast<double>(Stand.footprint.size());
                    Center.y_m /= static_cast<double>(Stand.footprint.size());
                    const FVector LabelPosition = ToLocal({Center.x_m, 305.0}, 45.0);
                    auto* Label = NewObject<UTextRenderComponent>(this, *FString::Printf(TEXT("DemoStand_%s"), UTF8_TO_TCHAR(Stand.id.c_str())));
                    Label->SetupAttachment(SceneRoot);
                    Label->SetText(FText::FromString(UTF8_TO_TCHAR(Stand.id.c_str())));
                    Label->SetHorizontalAlignment(EHorizTextAligment::EHTA_Center);
                    Label->SetWorldSize(600.0f);
                    Label->SetTextRenderColor(FColor(240, 239, 222));
                    Label->SetWorldLocation(LabelPosition);
                    Label->SetWorldRotation(FRotator(0.0f, 180.0f, 0.0f));
                    Label->SetCollisionEnabled(ECollisionEnabled::NoCollision);
                    Label->RegisterComponent();
                    AirportMarkings.Add(Label);
                    AddLocalSegment(FString(TEXT("DemoStandLeadIn_")) + UTF8_TO_TCHAR(Stand.id.c_str()),
                        {Center.x_m, 180.0}, {Center.x_m, 245.0}, 0.35, Marking, 30.0);
                }
                for (const auto& Area : Design.support_areas) {
                    if (Area.footprint.empty()) continue;
                    const FString Id = UTF8_TO_TCHAR(Area.id.c_str());
                    const FVector2D Center = AddFootprint(TEXT("DemoSupportPad_") + Id, Area.footprint, Apron, 16.0);
                    const auto Bounds = [&Area]() {
                        double MinX = Area.footprint.front().x_m, MaxX = MinX, MinY = Area.footprint.front().y_m, MaxY = MinY;
                        for (const auto& Point : Area.footprint) { MinX = FMath::Min(MinX, Point.x_m); MaxX = FMath::Max(MaxX, Point.x_m); MinY = FMath::Min(MinY, Point.y_m); MaxY = FMath::Max(MaxY, Point.y_m); }
                        return FVector2D(MaxX - MinX, MaxY - MinY);
                    }();
                    const FVector Location(Center.X * 100.0, -Center.Y * 100.0, 500.0);
                    if (Area.kind == "fuel_staging") {
                        for (int32 Bay = 0; Bay < 4; ++Bay) {
                            const FVector BayLocation(Center.X * 100.0 + (Bay - 1.5) * 1250.0, -Center.Y * 100.0, 48.0);
                            AddBox(TEXT("DemoFuelStagingBay"), BayLocation, FVector(9.0, 0.25, 0.12), 0.0f, Marking);
                        }
                        continue;
                    }
                    const FVector Size(Bounds.X * (Area.kind == "terminal_apron" ? 0.72 : 0.64),
                        Bounds.Y * 0.30, Area.kind == "terminal_apron" ? 10.0 : 8.0);
                    AddBox(FString(TEXT("DemoBuilding_")) + Id, Location, Size, 0.0f, Building);
                    AddBox(FString(TEXT("DemoBuildingRoof_")) + Id, Location + FVector(0, 0, Size.Z * 55.0),
                        FVector(Size.X + 5.0, Size.Y + 5.0, 1.2), 0.0f, BuildingRoof);
                    if (Area.kind == "terminal_apron") {
                        AddBox(TEXT("DemoTerminalGlazing"), Location + FVector(0, -Size.Y * 0.42, 40.0),
                            FVector(Size.X * 0.82, 0.6, Size.Z * 0.30), 0.0f,
                            CreateMaterial(FLinearColor(0.18f, 0.34f, 0.39f)));
                    }
                }
                const auto PoleMaterial = CreateMaterial(FLinearColor(0.19f, 0.21f, 0.20f));
                const auto LampMaterial = CreateMaterial(FLinearColor(0.72f, 0.62f, 0.40f));
                for (const double X : {-520.0, 520.0}) for (const double Y : {338.0, 450.0}) {
                    const FString PoleName = FString::Printf(TEXT("DemoRampLight_%d_%d"), static_cast<int32>(X), static_cast<int32>(Y));
                    const FVector PoleBase = ToLocal({X, Y});
                    AddBox(PoleName + TEXT("_Mast"), PoleBase + FVector(0, 0, 900), FVector(0.34, 0.34, 18.0), 0.0f, PoleMaterial);
                    AddBox(PoleName + TEXT("_Arm"), PoleBase + FVector(0, -220, 1730), FVector(0.34, 4.4, 0.26), 0.0f, PoleMaterial);
                    AddBox(PoleName + TEXT("_Fixture"), PoleBase + FVector(0, -420, 1670), FVector(0.95, 0.62, 0.35), 0.0f, LampMaterial);
                    auto* Light = NewObject<UPointLightComponent>(this, *PoleName);
                    Light->SetupAttachment(SceneRoot);
                    Light->SetMobility(EComponentMobility::Movable);
                    Light->SetIntensity(18000.0f);
                    Light->SetAttenuationRadius(4200.0f);
                    Light->SetLightColor(FColor(255, 224, 184));
                    Light->SetCastShadows(false);
                    Light->SetWorldLocation(PoleBase + FVector(0, -420, 1640));
                    Light->RegisterComponent();
                }
                UE_LOG(LogRampLab, Display, TEXT("Demo Airport environment built from simulator design: regions=%d stands=%d support=%d runway=%d; local meters at 100 cm/m"),
                    static_cast<int32>(Design.regions.size()), static_cast<int32>(Design.stands.size()),
                    static_cast<int32>(Design.support_areas.size()), static_cast<int32>(Design.runways.size()));
                return;
            }
        }
    }

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
            const FVector A = ToOverlay(X1, Y1) + FVector(0.0, 0.0, 25.0);
            const FVector B = ToOverlay(X2, Y2) + FVector(0.0, 0.0, 25.0);
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
            if (!bGeospatialWireframe) {
                const double HalfWidth = Width * 0.5;
                for (const double Side : {-1.0, 1.0}) {
                    const double OffsetX = -AxisY * (HalfWidth - 1.0) * Side;
                    const double OffsetY = AxisX * (HalfWidth - 1.0) * Side;
                    AddSegment(RunwayName + (Side < 0.0 ? TEXT("_EdgeA") : TEXT("_EdgeB")),
                        CenterX - AxisX * HalfLength + OffsetX, CenterY - AxisY * HalfLength + OffsetY,
                        CenterX + AxisX * HalfLength + OffsetX, CenterY + AxisY * HalfLength + OffsetY, 0.75, Marking);
                }
                for (double Along = -HalfLength + 45.0; Along < HalfLength - 30.0; Along += 60.0) {
                    AddSegment(RunwayName + TEXT("_CenterlineDash"), CenterX + AxisX * Along, CenterY + AxisY * Along,
                        CenterX + AxisX * FMath::Min(Along + 30.0, HalfLength - 20.0),
                        CenterY + AxisY * FMath::Min(Along + 30.0, HalfLength - 20.0), 0.45, Marking);
                }
                const int32 StripeCount = Width > 25.0 ? 6 : 4;
                for (const double End : {-1.0, 1.0}) {
                    const double ThresholdAlong = End * (HalfLength - 8.0);
                    for (int32 Stripe = 0; Stripe < StripeCount; ++Stripe) {
                        const double Across = (static_cast<double>(Stripe) - (StripeCount - 1) * 0.5) * (Width - 5.0) / StripeCount;
                        const double OffsetX = -AxisY * Across;
                        const double OffsetY = AxisX * Across;
                        AddSegment(RunwayName + TEXT("_Threshold"), CenterX + AxisX * ThresholdAlong + OffsetX,
                            CenterY + AxisY * ThresholdAlong + OffsetY,
                            CenterX + AxisX * (ThresholdAlong + End * 1.5) + OffsetX,
                            CenterY + AxisY * (ThresholdAlong + End * 1.5) + OffsetY, 1.0, Marking);
                    }
                }
                FString FirstDesignation, SecondDesignation;
                if (Identifier.Split(TEXT("/"), &FirstDesignation, &SecondDesignation)) {
                    const double TextOffset = HalfLength - 72.0;
                    for (const auto& Designation : {TPair<FString, double>(FirstDesignation, -1.0), TPair<FString, double>(SecondDesignation, 1.0)}) {
                        const double Along = Designation.Value * TextOffset;
                        const FVector TextPosition = ToOverlay(CenterX + AxisX * Along, CenterY + AxisY * Along) + FVector(0.0, 0.0, 65.0);
                        const FVector TextAhead = ToOverlay(CenterX + AxisX * (Along + Designation.Value), CenterY + AxisY * (Along + Designation.Value));
                        const FVector TextDirection = TextAhead - TextPosition;
                        auto* Number = NewObject<UTextRenderComponent>(this, *FString::Printf(TEXT("KAUO_RunwayDesignation_%s"), *Designation.Key));
                        Number->SetupAttachment(SceneRoot);
                        Number->SetText(FText::FromString(Designation.Key));
                        Number->SetHorizontalAlignment(EHorizTextAligment::EHTA_Center);
                        Number->SetVerticalAlignment(EVerticalTextAligment::EVRTA_TextCenter);
                        Number->SetWorldSize(520.0f);
                        Number->SetTextRenderColor(FColor(245, 244, 231));
                        Number->SetWorldLocation(TextPosition);
                        Number->SetWorldRotation(FRotator(0.0f, FMath::RadiansToDegrees(FMath::Atan2(TextDirection.Y, TextDirection.X)), 0.0f));
                        Number->SetCollisionEnabled(ECollisionEnabled::NoCollision);
                        Number->RegisterComponent();
                        AirportMarkings.Add(Number);
                    }
                }
            }
        }

        const auto ApronCenter = Features->GetObjectField(TEXT("apron_center_local_m"));
        const auto ApronSize = Features->GetObjectField(TEXT("apron_size_m"));
        if (ApronCenter.IsValid() && ApronSize.IsValid()) {
            double X = 0.0, Y = 0.0, Width = 0.0, Length = 0.0;
            if (ApronCenter->TryGetNumberField(TEXT("x"), X) && ApronCenter->TryGetNumberField(TEXT("y"), Y)
                && ApronSize->TryGetNumberField(TEXT("x"), Width) && ApronSize->TryGetNumberField(TEXT("y"), Length)) {
                // Keep the source imagery visible: the chart-graticule apron
                // bounds are approximate, so draw an outline rather than an
                // opaque slab that could imply a surveyed pavement boundary.
                const double HalfWidth = Width * 0.5;
                const double HalfLength = Length * 0.5;
                AddSegment(TEXT("KAUO_TerminalFboApron_North"), X - HalfWidth, Y + HalfLength,
                    X + HalfWidth, Y + HalfLength, 0.8, Marking);
                AddSegment(TEXT("KAUO_TerminalFboApron_South"), X - HalfWidth, Y - HalfLength,
                    X + HalfWidth, Y - HalfLength, 0.8, Marking);
                AddSegment(TEXT("KAUO_TerminalFboApron_East"), X + HalfWidth, Y - HalfLength,
                    X + HalfWidth, Y + HalfLength, 0.8, Marking);
                AddSegment(TEXT("KAUO_TerminalFboApron_West"), X - HalfWidth, Y - HalfLength,
                    X - HalfWidth, Y + HalfLength, 0.8, Marking);
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
            const FString BuildingName = Name.Contains(TEXT("hangar"), ESearchCase::IgnoreCase) ? TEXT("Hangar") : TEXT("FBO");
            AddBox(TEXT("KAUO_Building_") + FString::FromInt(Index), ToOverlay(X, Y) + FVector(0.0, 0.0, 375.0), FVector(Width, Length, 7.5),
                Heading - 90.0f, Building);
            AddBox(TEXT("KAUO_BuildingRoof_") + FString::FromInt(Index), ToOverlay(X, Y) + FVector(0.0, 0.0, 820.0), FVector(Width + 1.5, Length + 1.5, 1.4),
                Heading - 90.0f, BuildingRoof);
            const FVector FacadeOffset = ToOverlay(X, Y - Length * 0.27) + FVector(0.0, 0.0, 400.0);
            AddBox(TEXT("KAUO_BuildingFacade_") + FString::FromInt(Index), FacadeOffset, FVector(Width * 0.74, 0.5, 3.0),
                Heading - 90.0f, CreateMaterial(BuildingName == TEXT("FBO") ? FLinearColor(0.10f, 0.25f, 0.29f) : FLinearColor(0.26f, 0.29f, 0.30f)));
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
            // The graph is an approximate operational route network. Render
            // its centerline only so it reads as guidance without inventing
            // pavement edges over the aerial basemap.
            AddSegment(TEXT("KAUO_TaxiCenter_") + Id, A.X, A.Y, B.X, B.Y, 0.65, TaxiMarking);
        }
        for (const auto& Node : NodePositions) {
            if (!Node.Key.Contains(TEXT("hold"), ESearchCase::IgnoreCase)) continue;
            for (const auto& EdgeValue : Edges) {
                const auto Edge = EdgeValue->AsObject();
                if (!Edge.IsValid()) continue;
                FString From, To;
                if (!Edge->TryGetStringField(TEXT("from"), From) || !Edge->TryGetStringField(TEXT("to"), To)) continue;
                const FString Other = From == Node.Key ? To : To == Node.Key ? From : FString();
                if (Other.IsEmpty() || !NodePositions.Contains(Other)) continue;
                FVector2D Direction = NodePositions[Other] - Node.Value;
                if (!Direction.Normalize()) continue;
                const FVector2D Across(-Direction.Y, Direction.X);
                for (int32 Bar = 0; Bar < 4; ++Bar) {
                    const FVector2D Center = Node.Value + Direction * (static_cast<double>(Bar) * 0.65);
                    const FVector2D A = Center - Across * 6.0;
                    const FVector2D B = Center + Across * 6.0;
                    AddSegment(TEXT("KAUO_HoldShort_") + Node.Key, A.X, A.Y, B.X, B.Y, 0.45, TaxiMarking);
                }
                break;
            }
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
