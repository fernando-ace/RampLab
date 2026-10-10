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
    BuildGeographicContext();
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
                    if (auto* Subsystem = GameInstance->GetSubsystem<URampLabSimulationSubsystem>())
                        Subsystem->SetCameraPreset(ScreenshotCameraPreset);
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
