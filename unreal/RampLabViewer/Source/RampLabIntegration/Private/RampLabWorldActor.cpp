#include "RampLabWorldActor.h"

#include "RampLabIntegration.h"
#include "RampLabSimulationSubsystem.h"
#include "airside/integration/visualization.hpp"

#include "Components/SceneComponent.h"
#include "Camera/CameraComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/GameInstance.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "InputCoreTypes.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UObject/ConstructorHelpers.h"
#include "UnrealClient.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

namespace {

const airside::RoadNodeSnapshot* FindNode(const airside::SimulationSnapshot& Snapshot, airside::NodeId Id)
{
    const auto Match = std::ranges::find(Snapshot.road_nodes, Id, &airside::RoadNodeSnapshot::id);
    return Match == Snapshot.road_nodes.end() ? nullptr : &*Match;
}

FString VehicleStateText(airside::VehicleState State)
{
    switch (State) {
    case airside::VehicleState::Idle: return TEXT("Idle");
    case airside::VehicleState::Assigned: return TEXT("Assigned");
    case airside::VehicleState::TravelingToAircraft: return TEXT("Traveling");
    case airside::VehicleState::Servicing: return TEXT("Servicing");
    case airside::VehicleState::ReturningToDepot: return TEXT("Returning");
    }
    return TEXT("Unknown");
}

FString AircraftStateText(airside::AircraftState State)
{
    switch (State) {
    case airside::AircraftState::Scheduled: return TEXT("Scheduled");
    case airside::AircraftState::Arriving: return TEXT("Arriving");
    case airside::AircraftState::AtGate: return TEXT("At Gate");
    case airside::AircraftState::WaitingForServices: return TEXT("Waiting for Services");
    case airside::AircraftState::ReadyForPushback: return TEXT("Ready");
    case airside::AircraftState::Departed: return TEXT("Departed");
    }
    return TEXT("Unknown");
}

}  // namespace

ARampLabWorldActor::ARampLabWorldActor()
{
    PrimaryActorTick.bCanEverTick = true;
    bFindCameraComponentWhenViewTarget = true;
    SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
    RootComponent = SceneRoot;
    CameraComponent = CreateDefaultSubobject<UCameraComponent>(TEXT("OverviewCamera"));
    CameraComponent->SetupAttachment(SceneRoot);
    CameraComponent->SetProjectionMode(ECameraProjectionMode::Perspective);
    CameraComponent->SetFieldOfView(48.0f);
    CameraComponent->PostProcessBlendWeight = 1.0f;
    CameraComponent->PostProcessSettings.bOverride_AutoExposureMinBrightness = true;
    CameraComponent->PostProcessSettings.bOverride_AutoExposureMaxBrightness = true;
    CameraComponent->PostProcessSettings.AutoExposureMinBrightness = 1.0f;
    CameraComponent->PostProcessSettings.AutoExposureMaxBrightness = 1.0f;

    static ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(TEXT("/Engine/BasicShapes/Cube.Cube"));
    CubeMesh = Cube.Object;
    static ConstructorHelpers::FObjectFinder<UStaticMesh> Cylinder(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
    CylinderMesh = Cylinder.Object;
    static ConstructorHelpers::FObjectFinder<UMaterialInterface> BasicMaterial(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
    BaseMaterial = BasicMaterial.Object;
}

void ARampLabWorldActor::BeginPlay()
{
    Super::BeginPlay();
    Placement = FRampLabAirportPlacement::Load();
    RoadOpenMaterial = CreateMaterial(FLinearColor(0.015f, 0.020f, 0.024f));
    RoadClosedMaterial = CreateMaterial(FLinearColor(0.36f, 0.008f, 0.004f));
    ActiveRouteMaterial = CreateMaterial(FLinearColor(0.008f, 0.18f, 0.34f));
    GateOpenMaterial = CreateMaterial(FLinearColor(0.010f, 0.13f, 0.045f));
    GateOccupiedMaterial = CreateMaterial(FLinearColor(0.34f, 0.16f, 0.008f));
    AircraftMaterial = CreateMaterial(FLinearColor(0.26f, 0.30f, 0.34f));
    AircraftWaitingMaterial = CreateMaterial(FLinearColor(0.38f, 0.18f, 0.012f));
    AircraftReadyMaterial = CreateMaterial(FLinearColor(0.025f, 0.24f, 0.055f));
    FuelMaterial = CreateMaterial(FLinearColor(0.42f, 0.075f, 0.004f));
    BaggageMaterial = CreateMaterial(FLinearColor(0.005f, 0.16f, 0.30f));
    bCaptureRun = FParse::Param(FCommandLine::Get(), TEXT("RampLabCapture"));
    ApplyCameraPreset(TEXT("Overview"));
}

void ARampLabWorldActor::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    RuntimeWallSeconds += DeltaSeconds;
    ++RuntimeFrames;
    UpdateCamera(DeltaSeconds);
    if (auto* Controller = GetWorld()->GetFirstPlayerController();
        Controller != nullptr && Controller->GetViewTarget() != this) {
        Controller->SetViewTarget(this);
    }
    const auto* GameInstance = GetGameInstance();
    auto* Subsystem = GameInstance == nullptr ? nullptr : GameInstance->GetSubsystem<URampLabSimulationSubsystem>();
    const auto* Snapshot = Subsystem == nullptr ? nullptr : Subsystem->GetSnapshot();
    if (Snapshot == nullptr) return;
    if (BuiltScenario != Subsystem->GetScenarioName()) {
        ClearTopology();
        BuiltScenario = Subsystem->GetScenarioName();
        bTopologyBuilt = false;
        bPerformanceReported = false;
    }
    if (!bTopologyBuilt) BuildTopology(*Snapshot);
    Reconcile(*Snapshot, DeltaSeconds);
    if (Subsystem->IsDemoMode() && !bCaptureRun) {
        const int64 Time = Subsystem->GetPlaybackTime().count();
        int32 DesiredStage = 0;
        FString DesiredPreset(TEXT("Overview"));
        if (Subsystem->GetScenarioName().Equals(TEXT("baseline"), ESearchCase::IgnoreCase)) {
            if (Time >= 3000) { DesiredStage = 4; DesiredPreset = TEXT("Ramp"); }
            else if (Time >= 840) { DesiredStage = 3; DesiredPreset = TEXT("Service Roads"); }
            else if (Time >= 300) { DesiredStage = 2; DesiredPreset = TEXT("Service Roads"); }
            else if (Time >= 180) { DesiredStage = 1; DesiredPreset = TEXT("Ramp"); }
        } else {
            DesiredStage = 5;
            DesiredPreset = TEXT("Overview");
        }
        if (DesiredStage != DemoCameraStage) {
            DemoCameraStage = DesiredStage;
            Subsystem->SetCameraPreset(DesiredPreset);
            ApplyCameraPreset(DesiredPreset);
        }
    }
    MaybeCapture();
    if (CaptureExitCountdown >= 0.0f) {
        CaptureExitCountdown -= DeltaSeconds;
        if (CaptureExitCountdown <= 0.0f) {
            CaptureExitCountdown = -1.0f;
            UE_LOG(LogRampLab, Display, TEXT("RampLab capture sequence complete; exiting after screenshot flush."));
            FPlatformMisc::RequestExit(false);
        }
    }
    if (Subsystem->IsFinished() && !bPerformanceReported) {
        int32 ActorCount = 0;
        for (TActorIterator<AActor> It(GetWorld()); It; ++It) ++ActorCount;
        const double ApproximateFps = RuntimeWallSeconds > 0.0 ? static_cast<double>(RuntimeFrames) / RuntimeWallSeconds : 0.0;
        UE_LOG(LogRampLab, Display, TEXT("RampLab runtime sanity: scenario=%s approximate_fps=%.1f actor_count=%d cesium_loading_is_asynchronous=true"),
            *Subsystem->GetScenarioName(), ApproximateFps, ActorCount);
        bPerformanceReported = true;
    }
}

void ARampLabWorldActor::CalcCamera(float DeltaTime, FMinimalViewInfo& OutResult)
{
    CameraComponent->GetCameraView(DeltaTime, OutResult);
}

void ARampLabWorldActor::BuildTopology(const airside::SimulationSnapshot& Snapshot)
{
    for (const auto& Road : Snapshot.roads) {
        if (RoadMeshes.Contains(Road.id.value())) continue;
        const auto* Source = FindNode(Snapshot, Road.source);
        const auto* Destination = FindNode(Snapshot, Road.destination);
        if (Source == nullptr || Destination == nullptr) continue;

        auto* Mesh = CreateMeshComponent(TEXT("Road"), Road.id.value(), CubeMesh);
        const FVector Start = ToWorld(Source->position_m, 8.0f);
        const FVector End = ToWorld(Destination->position_m, 8.0f);
        const FVector Delta = End - Start;
        Mesh->SetWorldLocation((Start + End) * 0.5f);
        Mesh->SetWorldRotation(FRotator(0.0f, FMath::RadiansToDegrees(FMath::Atan2(Delta.Y, Delta.X)), 0.0f));
        Mesh->SetWorldScale3D(FVector(Delta.Size2D() / 100.0f, 4.5f, 0.10f));
        Mesh->SetMaterial(0, Road.enabled ? RoadOpenMaterial : RoadClosedMaterial);
        RoadMeshes.Add(Road.id.value(), Mesh);

        auto* Barrier = CreateMeshComponent(TEXT("ClosureBarrier"), Road.id.value(), CubeMesh);
        Barrier->SetWorldLocation((Start + End) * 0.5f + FVector(0.0f, 0.0f, 70.0f));
        Barrier->SetWorldRotation(Mesh->GetComponentRotation());
        Barrier->SetWorldScale3D(FVector(0.6f, 5.6f, 0.8f));
        Barrier->SetMaterial(0, RoadClosedMaterial);
        Barrier->SetHiddenInGame(Road.enabled);
        ClosureBarriers.Add(Road.id.value(), Barrier);
    }

    for (const auto& Gate : Snapshot.gates) {
        if (GateActors.Contains(Gate.id.value())) continue;
        auto* Actor = CreateMirrorActor(TEXT("Gate"), Gate.id.value(), CubeMesh);
        Actor->SetActorLocation(ToWorld(Gate.position_m, 0.0f));
        Actor->SetActorScale3D(FVector(8.0f, 8.0f, 0.35f));
        Actor->GetStaticMeshComponent()->SetMaterial(0, Gate.available ? GateOpenMaterial : GateOccupiedMaterial);
        GateActors.Add(Gate.id.value(), Actor);

        auto* Label = CreateLabel(TEXT("GateLabel"), Gate.id.value());
        Label->SetText(FText::FromString(UTF8_TO_TCHAR(Gate.name.c_str())));
        Label->SetWorldLocation(ToWorld(Gate.position_m, 500.0f));
        GateLabels.Add(Gate.id.value(), Label);
    }

    for (const auto& Aircraft : Snapshot.aircraft) {
        if (AircraftActors.Contains(Aircraft.id.value())) continue;
        auto* Actor = CreateMirrorActor(TEXT("Aircraft"), Aircraft.id.value(), CylinderMesh);
        Actor->SetActorScale3D(FVector(1.9f, 1.9f, 13.0f));
        Actor->SetActorRotation(FRotator(90.0f, 0.0f, 0.0f));
        Actor->GetStaticMeshComponent()->SetMaterial(0, AircraftMaterial);
        AircraftActors.Add(Aircraft.id.value(), Actor);

        auto* Wings = CreateMirrorActor(TEXT("AircraftWings"), Aircraft.id.value(), CubeMesh);
        Wings->SetActorScale3D(FVector(7.0f, 12.0f, 0.35f));
        Wings->GetStaticMeshComponent()->SetMaterial(0, AircraftMaterial);
        AircraftWingActors.Add(Aircraft.id.value(), Wings);

        auto* Label = CreateLabel(TEXT("AircraftLabel"), Aircraft.id.value());
        Label->SetText(FText::FromString(UTF8_TO_TCHAR(Aircraft.flight_number.c_str())));
        AircraftLabels.Add(Aircraft.id.value(), Label);
    }

    for (const auto& Vehicle : Snapshot.vehicles) {
        if (VehicleActors.Contains(Vehicle.id.value())) continue;
        auto* Actor = CreateMirrorActor(TEXT("Vehicle"), Vehicle.id.value(), CubeMesh);
        const bool bFuel = Vehicle.type == airside::ServiceType::Fueling;
        Actor->SetActorScale3D(bFuel ? FVector(6.5f, 3.4f, 1.7f) : FVector(4.0f, 3.0f, 1.4f));
        Actor->GetStaticMeshComponent()->SetMaterial(0, bFuel ? FuelMaterial : BaggageMaterial);
        VehicleActors.Add(Vehicle.id.value(), Actor);

        auto* Detail = CreateMirrorActor(TEXT("VehicleDetail"), Vehicle.id.value(), bFuel ? CylinderMesh : CubeMesh);
        Detail->SetActorScale3D(bFuel ? FVector(2.0f, 2.0f, 4.5f) : FVector(5.0f, 2.4f, 0.9f));
        if (bFuel) Detail->SetActorRotation(FRotator(90.0f, 0.0f, 0.0f));
        Detail->GetStaticMeshComponent()->SetMaterial(0, bFuel ? AircraftMaterial : BaggageMaterial);
        VehicleDetailActors.Add(Vehicle.id.value(), Detail);

        auto* Label = CreateLabel(TEXT("VehicleLabel"), Vehicle.id.value());
        VehicleLabels.Add(Vehicle.id.value(), Label);
    }
    bTopologyBuilt = true;
    UE_LOG(LogTemp, Display, TEXT("RampLab topology built: %d roads, %d gates, %d aircraft, %d vehicles"),
        RoadMeshes.Num(), GateActors.Num(), AircraftActors.Num(), VehicleActors.Num());
}

void ARampLabWorldActor::ClearTopology()
{
    // Actor/component destruction is deferred until the end of the frame. Give
    // the replacement topology a fresh namespace so a same-frame scenario
    // switch never collides with objects that are pending destruction.
    ++TopologyGeneration;
    for (auto& Pair : RoadMeshes) if (Pair.Value) Pair.Value->DestroyComponent();
    for (auto& Pair : ClosureBarriers) if (Pair.Value) Pair.Value->DestroyComponent();
    for (auto& Pair : GateLabels) if (Pair.Value) Pair.Value->DestroyComponent();
    for (auto& Pair : AircraftLabels) if (Pair.Value) Pair.Value->DestroyComponent();
    for (auto& Pair : VehicleLabels) if (Pair.Value) Pair.Value->DestroyComponent();
    for (auto& Pair : GateActors) if (Pair.Value) Pair.Value->Destroy();
    for (auto& Pair : AircraftActors) if (Pair.Value) Pair.Value->Destroy();
    for (auto& Pair : AircraftWingActors) if (Pair.Value) Pair.Value->Destroy();
    for (auto& Pair : VehicleActors) if (Pair.Value) Pair.Value->Destroy();
    for (auto& Pair : VehicleDetailActors) if (Pair.Value) Pair.Value->Destroy();
    RoadMeshes.Empty();
    ClosureBarriers.Empty();
    GateLabels.Empty();
    AircraftLabels.Empty();
    VehicleLabels.Empty();
    GateActors.Empty();
    AircraftActors.Empty();
    AircraftWingActors.Empty();
    VehicleActors.Empty();
    VehicleDetailActors.Empty();
}

void ARampLabWorldActor::Reconcile(const airside::SimulationSnapshot& Snapshot, float DeltaSeconds)
{
    for (const auto& Road : Snapshot.roads) {
        if (UStaticMeshComponent* Mesh = RoadMeshes.FindRef(Road.id.value()).Get()) {
            const auto* Subsystem = GetGameInstance()->GetSubsystem<URampLabSimulationSubsystem>();
            Mesh->SetMaterial(0, !Road.enabled
                ? RoadClosedMaterial
                : (Subsystem != nullptr && Subsystem->IsRoadOnSelectedRoute(Road.id.value()) ? ActiveRouteMaterial : RoadOpenMaterial));
        }
        if (UStaticMeshComponent* Barrier = ClosureBarriers.FindRef(Road.id.value()).Get()) {
            Barrier->SetHiddenInGame(Road.enabled);
        }
    }

    for (const auto& Gate : Snapshot.gates) {
        if (AStaticMeshActor* Actor = GateActors.FindRef(Gate.id.value()).Get()) {
            Actor->GetStaticMeshComponent()->SetMaterial(0, Gate.available ? GateOpenMaterial : GateOccupiedMaterial);
        }
        if (UTextRenderComponent* Label = GateLabels.FindRef(Gate.id.value()).Get()) {
            FString Occupant(TEXT("AVAILABLE"));
            if (Gate.occupying_aircraft) {
                const auto Aircraft = std::ranges::find(Snapshot.aircraft, *Gate.occupying_aircraft, &airside::AircraftSnapshot::id);
                Occupant = Aircraft == Snapshot.aircraft.end() ? TEXT("OCCUPIED") : UTF8_TO_TCHAR(Aircraft->flight_number.c_str());
            }
            Label->SetText(FText::FromString(FString::Printf(TEXT("%s  /  %s"), UTF8_TO_TCHAR(Gate.name.c_str()), *Occupant)));
        }
    }

    for (const auto& Aircraft : Snapshot.aircraft) {
        AStaticMeshActor* Actor = AircraftActors.FindRef(Aircraft.id.value()).Get();
        AStaticMeshActor* Wings = AircraftWingActors.FindRef(Aircraft.id.value()).Get();
        UTextRenderComponent* Label = AircraftLabels.FindRef(Aircraft.id.value()).Get();
        if (Actor == nullptr || Wings == nullptr || Label == nullptr) continue;
        const bool bVisible = Aircraft.state != airside::AircraftState::Scheduled && Aircraft.state != airside::AircraftState::Departed;
        Actor->SetActorHiddenInGame(!bVisible);
        Wings->SetActorHiddenInGame(!bVisible);
        Label->SetVisibility(bVisible);
        if (!bVisible) continue;

        const auto Gate = std::ranges::find(Snapshot.gates, Aircraft.assigned_gate, &airside::GateSnapshot::id);
        if (Gate == Snapshot.gates.end()) continue;
        const airside::Vec2 StandPosition{Gate->position_m.x_m - 14.0, Gate->position_m.y_m};
        const FVector Position = ToWorld(StandPosition, 260.0f);
        Actor->SetActorLocation(Position);
        Wings->SetActorLocation(Position - FVector(0.0f, 0.0f, 130.0f));
        const FRotator StandRotation(90.0f, -Placement.SimulationHeadingDegrees, 0.0f);
        Actor->SetActorRotation(StandRotation);
        Wings->SetActorRotation(FRotator(0.0f, -Placement.SimulationHeadingDegrees, 0.0f));
        Label->SetWorldLocation(Position + FVector(0.0f, 0.0f, 320.0f));
        Label->SetText(FText::FromString(FString::Printf(TEXT("%s  /  %s"),
            UTF8_TO_TCHAR(Aircraft.flight_number.c_str()), *AircraftStateText(Aircraft.state))));
        UMaterialInstanceDynamic* StateMaterial = Aircraft.state == airside::AircraftState::ReadyForPushback
            ? AircraftReadyMaterial
            : (Aircraft.state == airside::AircraftState::WaitingForServices ? AircraftWaitingMaterial : AircraftMaterial);
        Actor->GetStaticMeshComponent()->SetMaterial(0, StateMaterial);
        Wings->GetStaticMeshComponent()->SetMaterial(0, StateMaterial);
    }

    const auto* GameInstance = GetGameInstance();
    const auto* Subsystem = GameInstance == nullptr ? nullptr : GameInstance->GetSubsystem<URampLabSimulationSubsystem>();
    const auto PlaybackTime = Subsystem == nullptr ? Snapshot.simulation_time : Subsystem->GetPlaybackTime();
    for (const auto& Vehicle : Snapshot.vehicles) {
        AStaticMeshActor* Actor = VehicleActors.FindRef(Vehicle.id.value()).Get();
        AStaticMeshActor* Detail = VehicleDetailActors.FindRef(Vehicle.id.value()).Get();
        UTextRenderComponent* Label = VehicleLabels.FindRef(Vehicle.id.value()).Get();
        if (Actor == nullptr || Detail == nullptr || Label == nullptr) continue;

        airside::Vec2 PositionMeters{};
        bool bFound = false;
        TOptional<float> TargetYaw;
        if (Vehicle.journey) {
            if (const auto Sample = airside::visualization::sample_journey(*Vehicle.journey, Snapshot.road_nodes, PlaybackTime)) {
                PositionMeters = {Sample->position_m.x, Sample->position_m.y};
                const FVector Here = ToWorld(PositionMeters);
                const FVector Ahead = ToWorld({PositionMeters.x_m + Sample->direction.x, PositionMeters.y_m + Sample->direction.y});
                const FVector Direction = Ahead - Here;
                TargetYaw = FMath::RadiansToDegrees(FMath::Atan2(Direction.Y, Direction.X));
                bFound = true;
            }
        }
        if (!bFound) {
            if (const auto* Node = FindNode(Snapshot, Vehicle.current_node)) {
                PositionMeters = Node->position_m;
                bFound = true;
            }
        }
        if (!bFound) continue;

        const FVector Position = ToWorld(PositionMeters, 100.0f);
        Actor->SetActorLocation(Position);
        Detail->SetActorLocation(Position + FVector(0.0f, 0.0f, Vehicle.type == airside::ServiceType::Fueling ? 90.0f : -25.0f));
        if (TargetYaw) {
            const FRotator Smooth = FMath::RInterpTo(Actor->GetActorRotation(), FRotator(0.0f, *TargetYaw, 0.0f), DeltaSeconds, 5.0f);
            Actor->SetActorRotation(Smooth);
            Detail->SetActorRotation(Vehicle.type == airside::ServiceType::Fueling
                ? FRotator(90.0f, Smooth.Yaw, 0.0f) : Smooth);
        }
        Label->SetWorldLocation(Position + FVector(0.0f, 0.0f, 240.0f));
        Label->SetText(FText::FromString(FString::Printf(
            TEXT("%s - %s"), UTF8_TO_TCHAR(Vehicle.name.c_str()), *VehicleStateText(Vehicle.state))));
    }

    const FVector CameraLocation = CameraComponent->GetComponentLocation();
    for (const auto& Pair : GateLabels) if (Pair.Value) Pair.Value->SetWorldRotation((CameraLocation - Pair.Value->GetComponentLocation()).Rotation());
    for (const auto& Pair : AircraftLabels) if (Pair.Value) Pair.Value->SetWorldRotation((CameraLocation - Pair.Value->GetComponentLocation()).Rotation());
    for (const auto& Pair : VehicleLabels) if (Pair.Value) Pair.Value->SetWorldRotation((CameraLocation - Pair.Value->GetComponentLocation()).Rotation());
}

UStaticMeshComponent* ARampLabWorldActor::CreateMeshComponent(const FString& Prefix, uint32 Id, UStaticMesh* Mesh)
{
    auto* Component = NewObject<UStaticMeshComponent>(this, *FString::Printf(TEXT("%s_%u_G%u"), *Prefix, Id, TopologyGeneration));
    Component->SetupAttachment(SceneRoot);
    Component->SetMobility(EComponentMobility::Movable);
    Component->SetStaticMesh(Mesh);
    Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Component->RegisterComponent();
    return Component;
}

AStaticMeshActor* ARampLabWorldActor::CreateMirrorActor(const FString& Prefix, uint32 Id, UStaticMesh* Mesh)
{
    FActorSpawnParameters Parameters;
    Parameters.Name = *FString::Printf(TEXT("%s_%u_G%u"), *Prefix, Id, TopologyGeneration);
    auto* Actor = GetWorld()->SpawnActor<AStaticMeshActor>(FVector::ZeroVector, FRotator::ZeroRotator, Parameters);
    Actor->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
    Actor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
    Actor->GetStaticMeshComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    return Actor;
}

UTextRenderComponent* ARampLabWorldActor::CreateLabel(const FString& Prefix, uint32 Id)
{
    auto* Label = NewObject<UTextRenderComponent>(this, *FString::Printf(TEXT("%s_%u_G%u"), *Prefix, Id, TopologyGeneration));
    Label->SetupAttachment(SceneRoot);
    Label->SetHorizontalAlignment(EHorizTextAligment::EHTA_Center);
    Label->SetWorldSize(190.0f);
    Label->SetTextRenderColor(FColor::White);
    Label->SetWorldRotation(FRotator(0.0f, 0.0f, 0.0f));
    Label->RegisterComponent();
    return Label;
}

UMaterialInstanceDynamic* ARampLabWorldActor::CreateMaterial(const FLinearColor& Color)
{
    auto* Material = UMaterialInstanceDynamic::Create(
        BaseMaterial != nullptr ? BaseMaterial : UMaterial::GetDefaultMaterial(MD_Surface), this);
    Material->SetVectorParameterValue(TEXT("Color"), Color);
    Material->SetVectorParameterValue(TEXT("BaseColor"), Color);
    return Material;
}

void ARampLabWorldActor::MaybeCapture()
{
    if (!bCaptureRun || RuntimeWallSeconds < 14.5 || GEngine == nullptr || GEngine->GameViewport == nullptr) return;
    auto* Subsystem = GetGameInstance()->GetSubsystem<URampLabSimulationSubsystem>();
    if (Subsystem == nullptr) return;

    const int64 Time = Subsystem->GetPlaybackTime().count();
    struct FCapturePoint { const TCHAR* Scenario; int64 Time; const TCHAR* Name; const TCHAR* Camera; };
    const FCapturePoint Points[] = {
        {TEXT("baseline"), 0, TEXT("00-auburn-overview"), TEXT("Overview")},
        {TEXT("baseline"), 240, TEXT("01-baseline-operations"), TEXT("Ramp")},
        {TEXT("baseline"), 305, TEXT("02-road-closure"), TEXT("Service Roads")},
        {TEXT("baseline"), 900, TEXT("03-alternate-route"), TEXT("Service Roads")},
        {TEXT("baseline"), 3000, TEXT("04-baseline-results"), TEXT("Ramp")},
        {TEXT("high_capacity"), 2100, TEXT("05-high-capacity-comparison"), TEXT("Overview")},
    };
    if (CaptureStage >= UE_ARRAY_COUNT(Points)) return;
    const auto& Point = Points[CaptureStage];
    if (!Subsystem->GetScenarioName().Equals(Point.Scenario, ESearchCase::IgnoreCase) || Time < Point.Time) return;
    Subsystem->SetCameraPreset(Point.Camera);
    ApplyCameraPreset(Point.Camera);
    if (const auto* CaptureSnapshot = Subsystem->GetSnapshot()) Reconcile(*CaptureSnapshot, 0.0f);

    const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Screenshots"), TEXT("RampLab"));
    IFileManager::Get().MakeDirectory(*Directory, true);
    const FString Filename = FPaths::Combine(Directory, FString(Point.Name) + TEXT(".png"));
    FScreenshotRequest::RequestScreenshot(Filename, true, false, false, FIntRect(), true);
    ++CaptureStage;
    if (CaptureStage == UE_ARRAY_COUNT(Points)) CaptureExitCountdown = 2.0f;
}

FVector ARampLabWorldActor::ToWorld(airside::Vec2 Meters, float HeightCm) const
{
    return Placement.ToUnreal(Meters, HeightCm);
}

void ARampLabWorldActor::UpdateCamera(float DeltaSeconds)
{
    const auto* Subsystem = GetGameInstance() == nullptr ? nullptr : GetGameInstance()->GetSubsystem<URampLabSimulationSubsystem>();
    if (Subsystem != nullptr && AppliedCameraPreset != Subsystem->GetCameraPreset()) ApplyCameraPreset(Subsystem->GetCameraPreset());

    if (auto* Controller = GetWorld()->GetFirstPlayerController()) {
        const float Move = 18000.0f * DeltaSeconds;
        const FVector Forward = FRotationMatrix(FRotator(0.0f, CameraYaw, 0.0f)).GetUnitAxis(EAxis::X);
        const FVector Right = FRotationMatrix(FRotator(0.0f, CameraYaw, 0.0f)).GetUnitAxis(EAxis::Y);
        if (Controller->IsInputKeyDown(EKeys::W)) CameraFocus += Forward * Move;
        if (Controller->IsInputKeyDown(EKeys::S)) CameraFocus -= Forward * Move;
        if (Controller->IsInputKeyDown(EKeys::D)) CameraFocus += Right * Move;
        if (Controller->IsInputKeyDown(EKeys::A)) CameraFocus -= Right * Move;
        if (Controller->IsInputKeyDown(EKeys::Q)) CameraYaw -= 35.0f * DeltaSeconds;
        if (Controller->IsInputKeyDown(EKeys::E)) CameraYaw += 35.0f * DeltaSeconds;
        CameraDistance = FMath::Clamp(CameraDistance - Controller->GetInputAnalogKeyState(EKeys::MouseWheelAxis) * 5000.0f, 12000.0f, 140000.0f);
    }
    const FRotator Rotation(CameraPitch, CameraYaw, 0.0f);
    CameraComponent->SetWorldRotation(Rotation);
    CameraComponent->SetWorldLocation(CameraFocus - Rotation.Vector() * CameraDistance);
}

void ARampLabWorldActor::ApplyCameraPreset(const FString& Preset)
{
    AppliedCameraPreset = Preset;
    if (Preset == TEXT("Ramp")) {
        CameraFocus = ToWorld({120.0, 0.0}); CameraDistance = 40000.0f; CameraYaw = -42.0f; CameraPitch = -55.0f;
    } else if (Preset == TEXT("Gate A2")) {
        CameraFocus = ToWorld({240.0, 0.0}); CameraDistance = 18500.0f; CameraYaw = -60.0f; CameraPitch = -43.0f;
    } else if (Preset == TEXT("Service Roads")) {
        CameraFocus = ToWorld({120.0, 0.0}); CameraDistance = 36000.0f; CameraYaw = -18.0f; CameraPitch = -62.0f;
    } else {
        CameraFocus = FVector(15000.0f, 0.0f, 0.0f); CameraDistance = 82000.0f; CameraYaw = -38.0f; CameraPitch = -54.0f;
    }
    const FRotator Rotation(CameraPitch, CameraYaw, 0.0f);
    CameraComponent->SetWorldRotation(Rotation);
    CameraComponent->SetWorldLocation(CameraFocus - Rotation.Vector() * CameraDistance);
}
