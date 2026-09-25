#include "RampLabWorldActor.h"

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
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
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

}  // namespace

ARampLabWorldActor::ARampLabWorldActor()
{
    PrimaryActorTick.bCanEverTick = true;
    bFindCameraComponentWhenViewTarget = true;
    SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
    RootComponent = SceneRoot;
    CameraComponent = CreateDefaultSubobject<UCameraComponent>(TEXT("OverviewCamera"));
    CameraComponent->SetupAttachment(SceneRoot);
    CameraComponent->SetRelativeLocation(FVector(12000.0f, 0.0f, 50000.0f));
    CameraComponent->SetRelativeRotation(FRotator(-90.0f, 0.0f, 0.0f));
    CameraComponent->SetProjectionMode(ECameraProjectionMode::Orthographic);
    CameraComponent->SetOrthoWidth(48000.0f);

    static ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(TEXT("/Engine/BasicShapes/Cube.Cube"));
    CubeMesh = Cube.Object;
    static ConstructorHelpers::FObjectFinder<UMaterialInterface> BasicMaterial(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
    BaseMaterial = BasicMaterial.Object;
}

void ARampLabWorldActor::BeginPlay()
{
    Super::BeginPlay();
    RoadOpenMaterial = CreateMaterial(FLinearColor(0.08f, 0.10f, 0.13f));
    RoadClosedMaterial = CreateMaterial(FLinearColor(0.85f, 0.04f, 0.03f));
    GateOpenMaterial = CreateMaterial(FLinearColor(0.12f, 0.30f, 0.16f));
    GateOccupiedMaterial = CreateMaterial(FLinearColor(0.55f, 0.42f, 0.05f));
    AircraftMaterial = CreateMaterial(FLinearColor(0.10f, 0.35f, 0.95f));
    AircraftReadyMaterial = CreateMaterial(FLinearColor(0.15f, 0.85f, 0.25f));
    FuelMaterial = CreateMaterial(FLinearColor(1.00f, 0.35f, 0.04f));
    BaggageMaterial = CreateMaterial(FLinearColor(0.05f, 0.80f, 0.95f));
    bCaptureRun = FParse::Param(FCommandLine::Get(), TEXT("RampLabCapture"));
}

void ARampLabWorldActor::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (auto* Controller = GetWorld()->GetFirstPlayerController();
        Controller != nullptr && Controller->GetViewTarget() != this) {
        Controller->SetViewTarget(this);
    }
    const auto* GameInstance = GetGameInstance();
    const auto* Subsystem = GameInstance == nullptr ? nullptr : GameInstance->GetSubsystem<URampLabSimulationSubsystem>();
    const auto* Snapshot = Subsystem == nullptr ? nullptr : Subsystem->GetSnapshot();
    if (Snapshot == nullptr) return;
    if (!bTopologyBuilt) BuildTopology(*Snapshot);
    Reconcile(*Snapshot);
    MaybeCapture();
}

void ARampLabWorldActor::CalcCamera(float DeltaTime, FMinimalViewInfo& OutResult)
{
    CameraComponent->GetCameraView(DeltaTime, OutResult);
}

void ARampLabWorldActor::BuildTopology(const airside::SimulationSnapshot& Snapshot)
{
    for (const auto& Road : Snapshot.roads) {
        const auto* Source = FindNode(Snapshot, Road.source);
        const auto* Destination = FindNode(Snapshot, Road.destination);
        if (Source == nullptr || Destination == nullptr) continue;

        auto* Mesh = CreateMeshComponent(TEXT("Road"), Road.id.value(), CubeMesh);
        const FVector Start = ToWorld(Source->position_m, 8.0f);
        const FVector End = ToWorld(Destination->position_m, 8.0f);
        const FVector Delta = End - Start;
        Mesh->SetWorldLocation((Start + End) * 0.5f);
        Mesh->SetWorldRotation(FRotator(0.0f, FMath::RadiansToDegrees(FMath::Atan2(Delta.Y, Delta.X)), 0.0f));
        Mesh->SetWorldScale3D(FVector(Delta.Size2D() / 100.0f, 0.75f, 0.12f));
        Mesh->SetMaterial(0, Road.enabled ? RoadOpenMaterial : RoadClosedMaterial);
        RoadMeshes.Add(Road.id.value(), Mesh);
    }

    for (const auto& Gate : Snapshot.gates) {
        auto* Actor = CreateMirrorActor(TEXT("Gate"), Gate.id.value(), CubeMesh);
        Actor->SetActorLocation(ToWorld(Gate.position_m, 130.0f));
        Actor->SetActorScale3D(FVector(20.0f, 11.0f, 1.4f));
        Actor->GetStaticMeshComponent()->SetMaterial(0, Gate.available ? GateOpenMaterial : GateOccupiedMaterial);
        GateActors.Add(Gate.id.value(), Actor);

        auto* Label = CreateLabel(TEXT("GateLabel"), Gate.id.value());
        Label->SetText(FText::FromString(UTF8_TO_TCHAR(Gate.name.c_str())));
        Label->SetWorldLocation(ToWorld(Gate.position_m, 340.0f));
        GateLabels.Add(Gate.id.value(), Label);
    }

    for (const auto& Aircraft : Snapshot.aircraft) {
        auto* Actor = CreateMirrorActor(TEXT("Aircraft"), Aircraft.id.value(), CubeMesh);
        Actor->SetActorScale3D(FVector(16.0f, 6.5f, 1.8f));
        Actor->GetStaticMeshComponent()->SetMaterial(0, AircraftMaterial);
        AircraftActors.Add(Aircraft.id.value(), Actor);

        auto* Label = CreateLabel(TEXT("AircraftLabel"), Aircraft.id.value());
        Label->SetText(FText::FromString(UTF8_TO_TCHAR(Aircraft.flight_number.c_str())));
        AircraftLabels.Add(Aircraft.id.value(), Label);
    }

    for (const auto& Vehicle : Snapshot.vehicles) {
        auto* Actor = CreateMirrorActor(TEXT("Vehicle"), Vehicle.id.value(), CubeMesh);
        const bool bFuel = Vehicle.type == airside::ServiceType::Fueling;
        Actor->SetActorScale3D(bFuel ? FVector(7.0f, 4.0f, 1.6f) : FVector(6.0f, 3.5f, 1.3f));
        Actor->GetStaticMeshComponent()->SetMaterial(0, bFuel ? FuelMaterial : BaggageMaterial);
        VehicleActors.Add(Vehicle.id.value(), Actor);

        auto* Label = CreateLabel(TEXT("VehicleLabel"), Vehicle.id.value());
        VehicleLabels.Add(Vehicle.id.value(), Label);
    }
    bTopologyBuilt = true;
    UE_LOG(LogTemp, Display, TEXT("RampLab topology built: %d roads, %d gates, %d aircraft, %d vehicles"),
        RoadMeshes.Num(), GateActors.Num(), AircraftActors.Num(), VehicleActors.Num());
}

void ARampLabWorldActor::Reconcile(const airside::SimulationSnapshot& Snapshot)
{
    for (const auto& Road : Snapshot.roads) {
        if (UStaticMeshComponent* Mesh = RoadMeshes.FindRef(Road.id.value()).Get()) {
            Mesh->SetMaterial(0, Road.enabled ? RoadOpenMaterial : RoadClosedMaterial);
        }
    }

    for (const auto& Gate : Snapshot.gates) {
        if (AStaticMeshActor* Actor = GateActors.FindRef(Gate.id.value()).Get()) {
            Actor->GetStaticMeshComponent()->SetMaterial(0, Gate.available ? GateOpenMaterial : GateOccupiedMaterial);
        }
    }

    for (const auto& Aircraft : Snapshot.aircraft) {
        AStaticMeshActor* Actor = AircraftActors.FindRef(Aircraft.id.value()).Get();
        UTextRenderComponent* Label = AircraftLabels.FindRef(Aircraft.id.value()).Get();
        if (Actor == nullptr || Label == nullptr) continue;
        const bool bVisible = Aircraft.state != airside::AircraftState::Scheduled && Aircraft.state != airside::AircraftState::Departed;
        Actor->SetActorHiddenInGame(!bVisible);
        Label->SetVisibility(bVisible);
        if (!bVisible) continue;

        const auto Gate = std::ranges::find(Snapshot.gates, Aircraft.assigned_gate, &airside::GateSnapshot::id);
        if (Gate == Snapshot.gates.end()) continue;
        const FVector Position = ToWorld(Gate->position_m, 420.0f);
        Actor->SetActorLocation(Position);
        Label->SetWorldLocation(Position + FVector(0.0f, 0.0f, 320.0f));
        Actor->GetStaticMeshComponent()->SetMaterial(0,
            Aircraft.state == airside::AircraftState::ReadyForPushback ? AircraftReadyMaterial : AircraftMaterial);
    }

    const auto* GameInstance = GetGameInstance();
    const auto* Subsystem = GameInstance == nullptr ? nullptr : GameInstance->GetSubsystem<URampLabSimulationSubsystem>();
    const auto PlaybackTime = Subsystem == nullptr ? Snapshot.simulation_time : Subsystem->GetPlaybackTime();
    for (const auto& Vehicle : Snapshot.vehicles) {
        AStaticMeshActor* Actor = VehicleActors.FindRef(Vehicle.id.value()).Get();
        UTextRenderComponent* Label = VehicleLabels.FindRef(Vehicle.id.value()).Get();
        if (Actor == nullptr || Label == nullptr) continue;

        airside::Vec2 PositionMeters{};
        bool bFound = false;
        if (Vehicle.journey) {
            if (const auto Sample = airside::visualization::sample_journey(*Vehicle.journey, Snapshot.road_nodes, PlaybackTime)) {
                PositionMeters = {Sample->position_m.x, Sample->position_m.y};
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

        const FVector Position = ToWorld(PositionMeters, 360.0f);
        Actor->SetActorLocation(Position);
        Label->SetWorldLocation(Position + FVector(0.0f, 0.0f, 240.0f));
        Label->SetText(FText::FromString(FString::Printf(
            TEXT("%s - %s"), UTF8_TO_TCHAR(Vehicle.name.c_str()), *VehicleStateText(Vehicle.state))));
    }
}

UStaticMeshComponent* ARampLabWorldActor::CreateMeshComponent(const FString& Prefix, uint32 Id, UStaticMesh* Mesh)
{
    auto* Component = NewObject<UStaticMeshComponent>(this, *FString::Printf(TEXT("%s_%u"), *Prefix, Id));
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
    Parameters.Name = *FString::Printf(TEXT("%s_%u"), *Prefix, Id);
    auto* Actor = GetWorld()->SpawnActor<AStaticMeshActor>(FVector::ZeroVector, FRotator::ZeroRotator, Parameters);
    Actor->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
    Actor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
    Actor->GetStaticMeshComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    return Actor;
}

UTextRenderComponent* ARampLabWorldActor::CreateLabel(const FString& Prefix, uint32 Id)
{
    auto* Label = NewObject<UTextRenderComponent>(this, *FString::Printf(TEXT("%s_%u"), *Prefix, Id));
    Label->SetupAttachment(SceneRoot);
    Label->SetHorizontalAlignment(EHorizTextAligment::EHTA_Center);
    Label->SetWorldSize(260.0f);
    Label->SetTextRenderColor(FColor::White);
    Label->SetWorldRotation(FRotator(-90.0f, 0.0f, 0.0f));
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
    if (!bCaptureRun || GEngine == nullptr || GEngine->GameViewport == nullptr) return;
    const auto* Subsystem = GetGameInstance()->GetSubsystem<URampLabSimulationSubsystem>();
    if (Subsystem == nullptr) return;

    const int64 Time = Subsystem->GetPlaybackTime().count();
    const int64 Thresholds[] = {0, 305, 900, 2820};
    const TCHAR* Names[] = {TEXT("00-start"), TEXT("01-road-closed"), TEXT("02-reroute"), TEXT("03-complete")};
    if (CaptureStage >= UE_ARRAY_COUNT(Thresholds) || Time < Thresholds[CaptureStage]) return;

    const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Screenshots"), TEXT("RampLab"));
    IFileManager::Get().MakeDirectory(*Directory, true);
    const FString Filename = FPaths::Combine(Directory, FString(Names[CaptureStage]) + TEXT(".png"));
    FScreenshotRequest::RequestScreenshot(Filename, true, false, false, FIntRect(), true);
    ++CaptureStage;
}

FVector ARampLabWorldActor::ToWorld(airside::Vec2 Meters, float HeightCm) const
{
    constexpr airside::visualization::CoordinateTransform Transform{100.0, {0.0, 0.0}, false};
    const auto Position = Transform.apply(Meters);
    return FVector(Position.x, Position.y, HeightCm);
}
