#include "RampLabWorldActor.h"

#include "RampLabIntegration.h"
#include "RampLabAirportEnvironment.h"
#include "RampLabSimulationSubsystem.h"
#include "RampLabSensors.h"
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
#include "DrawDebugHelpers.h"
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

#include <cmath>

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

bool IsDemoAirport(const URampLabSimulationSubsystem* Subsystem)
{
    const auto* Snapshot = Subsystem == nullptr ? nullptr : Subsystem->GetSnapshot();
    return Snapshot != nullptr && Snapshot->airport_design.has_value()
        && Snapshot->airport_design->name == "RampLab Demo Airport";
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
    static ConstructorHelpers::FObjectFinder<UStaticMesh> Cone(TEXT("/Engine/BasicShapes/Cone.Cone"));
    ConeMesh = Cone.Object;
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
    AircraftRouteMaterial = CreateMaterial(FLinearColor(0.95f, 0.62f, 0.08f));
    GroundVehicleRouteMaterial = CreateMaterial(FLinearColor(0.08f, 0.58f, 0.84f));
    SharedRouteMaterial = CreateMaterial(FLinearColor(0.83f, 0.86f, 0.80f));
    RunwayCrossingRouteMaterial = CreateMaterial(FLinearColor(0.96f, 0.20f, 0.08f));
    GateOpenMaterial = CreateMaterial(FLinearColor(0.010f, 0.13f, 0.045f));
    GateOccupiedMaterial = CreateMaterial(FLinearColor(0.34f, 0.16f, 0.008f));
    AutonomyMaterial = CreateMaterial(FLinearColor(0.92f, 0.37f, 0.035f));
    GnssMaterial = CreateMaterial(FLinearColor(0.02f, 0.76f, 0.94f));
    ObstacleMaterial = CreateMaterial(FLinearColor(0.62f, 0.045f, 0.025f));
    AircraftMaterial = CreateMaterial(FLinearColor(0.72f, 0.77f, 0.82f));
    AircraftGearMaterial = CreateMaterial(FLinearColor(0.022f, 0.025f, 0.026f));
    AircraftWaitingMaterial = CreateMaterial(FLinearColor(0.76f, 0.80f, 0.84f));
    AircraftReadyMaterial = CreateMaterial(FLinearColor(0.025f, 0.24f, 0.055f));
    FuelMaterial = CreateMaterial(FLinearColor(0.42f, 0.075f, 0.004f));
    BaggageMaterial = CreateMaterial(FLinearColor(0.005f, 0.16f, 0.30f));
    bCaptureRun = FParse::Param(FCommandLine::Get(), TEXT("RampLabCapture"));
    const auto* Subsystem = GetGameInstance()->GetSubsystem<URampLabSimulationSubsystem>();
    const bool bDemoAirport = IsDemoAirport(Subsystem);
    ApplyCameraPreset(bDemoAirport ? TEXT("Airport Overview") : TEXT("KAUO Overview"));
}

void ARampLabWorldActor::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    RuntimeWallSeconds += DeltaSeconds;
    ++RuntimeFrames;
    MaximumFrameDeltaSeconds = FMath::Max(MaximumFrameDeltaSeconds, DeltaSeconds);
    if (DeltaSeconds > 0.5f) ++StreamingStallFrames;
    UpdateCamera(DeltaSeconds);
    if (auto* Controller = GetWorld()->GetFirstPlayerController();
        Controller != nullptr && Controller->GetViewTarget() != this) {
        Controller->SetViewTarget(this);
    }
    const auto* GameInstance = GetGameInstance();
    auto* Subsystem = GameInstance == nullptr ? nullptr : GameInstance->GetSubsystem<URampLabSimulationSubsystem>();
    const auto* Snapshot = Subsystem == nullptr ? nullptr : Subsystem->GetSnapshot();
    if (Snapshot == nullptr) return;
    if (RuntimeWallSeconds >= NextPerformanceReportSeconds) {
        const double ApproximateFps = RuntimeWallSeconds > 0.0 ? static_cast<double>(RuntimeFrames) / RuntimeWallSeconds : 0.0;
        FString CesiumState(TEXT("not initialized"));
        for (TActorIterator<ARampLabAirportEnvironment> It(GetWorld()); It; ++It) {
            CesiumState = It->GetCesiumStatus();
            break;
        }
        UE_LOG(LogRampLab, Display,
            TEXT("Goal29 runtime telemetry: approximate_fps=%.1f synchronized_aircraft=%d synchronized_vehicles=%d total_entities=%d mirror_rate_hz=10 cesium_status=\"%s\" frames_over_500ms=%u maximum_frame_delta_seconds=%.3f"),
            ApproximateFps, static_cast<int32>(Snapshot->aircraft.size()), static_cast<int32>(Snapshot->vehicles.size()),
            static_cast<int32>(Snapshot->aircraft.size() + Snapshot->vehicles.size()), *CesiumState,
            StreamingStallFrames, MaximumFrameDeltaSeconds);
        NextPerformanceReportSeconds += 10.0;
    }
    if (BuiltScenario != Subsystem->GetScenarioName()) {
        ClearTopology();
        BuiltScenario = Subsystem->GetScenarioName();
        bTopologyBuilt = false;
        bPerformanceReported = false;
    }
    if (!bTopologyBuilt) BuildTopology(*Snapshot);
    if (Subsystem->IsAutonomyMode()) {
        if(Subsystem->IsFleetMode()) ReconcileFleet(Subsystem->GetFleetSnapshots());
        else {
            const auto* Autonomy = Subsystem->GetAutonomySnapshot();
            if (Autonomy == nullptr) return;
            if (!bAutonomyTopologyBuilt) BuildAutonomyTopology(*Autonomy);
            ReconcileAutonomy(*Autonomy);
        }
    } else {
        bAutonomyTopologyBuilt = false;
        AutonomyTrail.Reset();
        Reconcile(*Snapshot, DeltaSeconds);
    }
    if (Subsystem->IsDemoMode() && !bCaptureRun) {
        const int64 Time = Subsystem->GetPlaybackTime().count();
        int32 DesiredStage = 0;
        FString DesiredPreset(TEXT("Overview"));
        if (Subsystem->GetScenarioName().Equals(TEXT("baseline"), ESearchCase::IgnoreCase)) {
            if (Time >= 3000) { DesiredStage = 4; DesiredPreset = TEXT("Ramp"); }
            else if (Time >= 840) { DesiredStage = 3; DesiredPreset = TEXT("Service Roads"); }
            else if (Time >= 300) { DesiredStage = 2; DesiredPreset = TEXT("Service Roads"); }
            else if (Time >= 180) { DesiredStage = 1; DesiredPreset = TEXT("Ramp"); }
        } else if (Subsystem->GetScenarioName().Equals(TEXT("high_capacity"), ESearchCase::IgnoreCase)) {
            DesiredStage = 5;
            DesiredPreset = TEXT("Overview");
        } else {
            DesiredStage = 6;
            DesiredPreset = TEXT("Service Roads");
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
        if (Subsystem->IsAutonomyMode()) {
            const auto* Autonomy = Subsystem->GetAutonomySnapshot();
            const double SimulationTime = Autonomy == nullptr ? 0.0 : Autonomy->timestamp_s;
            if (AutonomyLidarSensor != nullptr) {
                const double EffectiveRateHz = SimulationTime > 0.0
                    ? static_cast<double>(AutonomyLidarSensor->GetMeasuredCaptureCount()) / SimulationTime : 0.0;
                UE_LOG(LogRampLab, Display, TEXT("RampLab Unreal LiDAR metrics: frames=%llu effective_hz=%.2f rays=%d total_hits=%llu mean_capture_ms=%.4f"),
                    static_cast<unsigned long long>(AutonomyLidarSensor->GetMeasuredCaptureCount()), EffectiveRateHz,
                    AutonomyLidarSensor->RayCount,
                    static_cast<unsigned long long>(AutonomyLidarSensor->GetTotalHitReturns()),
                    AutonomyLidarSensor->GetMeanCaptureCostMilliseconds());
            }
            if (AutonomyCameraSensor != nullptr) {
                const double EffectiveRateHz = SimulationTime > 0.0
                    ? static_cast<double>(AutonomyCameraSensor->GetMeasuredCaptureCount()) / SimulationTime : 0.0;
                UE_LOG(LogRampLab, Display, TEXT("RampLab Unreal camera metrics: frames=%llu effective_hz=%.2f size=%dx%d mean_capture_ms=%.4f"),
                    static_cast<unsigned long long>(AutonomyCameraSensor->GetMeasuredCaptureCount()), EffectiveRateHz,
                    AutonomyCameraSensor->ImageWidth, AutonomyCameraSensor->ImageHeight,
                    AutonomyCameraSensor->GetMeanCaptureCostMilliseconds());
            }
        }
        bPerformanceReported = true;
    }
}

void ARampLabWorldActor::ReconcileFleet(const std::vector<airside::autonomy::FleetVehicleSnapshot>& Snapshots)
{
    for(auto& Pair:AircraftActors)if(Pair.Value)Pair.Value->SetActorHiddenInGame(true);
    for(auto& Pair:AircraftWingActors)if(Pair.Value)Pair.Value->SetActorHiddenInGame(true);
    for(auto& Pair:VehicleActors)if(Pair.Value)Pair.Value->SetActorHiddenInGame(true);
    for(auto& Pair:VehicleDetailActors)if(Pair.Value)Pair.Value->SetActorHiddenInGame(true);
    for(auto& Pair:AircraftLabels)if(Pair.Value)Pair.Value->SetVisibility(false);
    for(auto& Pair:VehicleLabels)if(Pair.Value)Pair.Value->SetVisibility(false);
    uint32 Index=0;
    for(const auto& Vehicle:Snapshots){
        const FString Id=UTF8_TO_TCHAR(Vehicle.id.value.c_str());
        if(!FleetVehicleActors.Contains(Id)){
            auto* Actor=CreateMirrorActor(TEXT("FleetTug"),900100+Index,CubeMesh);Actor->GetStaticMeshComponent()->SetMaterial(0,AutonomyMaterial);Actor->SetActorScale3D(FVector(5.0f,2.4f,1.8f));FleetVehicleActors.Add(Id,Actor);
            auto* Label=CreateLabel(TEXT("FleetTugLabel"),900100+Index);FleetVehicleLabels.Add(Id,Label);
        }
        const auto& Pose=Vehicle.autonomy.ground_truth;const FVector Position=ToWorld(Pose.position,100.0f);auto Actor=FleetVehicleActors[Id];Actor->SetActorLocation(Position);Actor->SetActorRotation(FRotator(0.0f,FMath::RadiansToDegrees(Pose.heading_rad),0.0f));
        auto Label=FleetVehicleLabels[Id];Label->SetWorldLocation(Position+FVector(0.0f,0.0f,260.0f));Label->SetText(FText::FromString(FString::Printf(TEXT("%s -> %s | %s"),*Id,UTF8_TO_TCHAR(Vehicle.goal_node.c_str()),Vehicle.waiting?TEXT("WAITING"):Vehicle.autonomy.finished?TEXT("COMPLETE"):Pose.speed_mps<0.15?TEXT("STOPPED"):TEXT("MOVING"))));
        ++Index;
    }
    const FVector CameraLocation=CameraComponent->GetComponentLocation();for(const auto& Pair:FleetVehicleLabels)if(Pair.Value)Pair.Value->SetWorldRotation((CameraLocation-Pair.Value->GetComponentLocation()).Rotation());
}

void ARampLabWorldActor::CalcCamera(float DeltaTime, FMinimalViewInfo& OutResult)
{
    CameraComponent->GetCameraView(DeltaTime, OutResult);
}

void ARampLabWorldActor::BuildTopology(const airside::SimulationSnapshot& Snapshot)
{
    const bool bDemoAirport = Snapshot.airport_design.has_value() && Snapshot.airport_design->name == "RampLab Demo Airport";
    const bool bShowRoutes = FParse::Param(FCommandLine::Get(), TEXT("RampLabShowRoutes"));
    for (const auto& Road : Snapshot.roads) {
        if (bDemoAirport && !bShowRoutes) continue;
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
        Mesh->SetWorldScale3D(FVector(Delta.Size2D() / 100.0f,
            bDemoAirport && Road.width_m > 0.0 ? Road.width_m : 4.5, 0.10f));
        UMaterialInstanceDynamic* PermissionMaterial = Road.crosses_runway ? RunwayCrossingRouteMaterial.Get()
            : Road.aircraft_allowed && !Road.ground_vehicle_allowed ? AircraftRouteMaterial.Get()
            : !Road.aircraft_allowed && Road.ground_vehicle_allowed ? GroundVehicleRouteMaterial.Get() : SharedRouteMaterial.Get();
        Mesh->SetMaterial(0, Road.enabled ? PermissionMaterial : RoadClosedMaterial.Get());
        RoadMeshes.Add(Road.id.value(), Mesh);

        auto* Barrier = CreateMeshComponent(TEXT("ClosureBarrier"), Road.id.value(), CubeMesh);
        Barrier->SetWorldLocation((Start + End) * 0.5f + FVector(0.0f, 0.0f, 70.0f));
        Barrier->SetWorldRotation(Mesh->GetComponentRotation());
        Barrier->SetWorldScale3D(FVector(0.6f, 5.6f, 0.8f));
        Barrier->SetMaterial(0, RoadClosedMaterial);
        Barrier->SetHiddenInGame(Road.enabled);
        ClosureBarriers.Add(Road.id.value(), Barrier);
    }

    if (bShowRoutes) for (const auto& Node : Snapshot.road_nodes) {
        auto* Marker = CreateMeshComponent(TEXT("RouteNode"), Node.id.value(), CylinderMesh);
        Marker->SetWorldLocation(ToWorld(Node.position_m, 42.0f));
        Marker->SetWorldScale3D(FVector(0.22f, 0.22f, 0.08f));
        Marker->SetMaterial(0, AircraftRouteMaterial);
        RouteNodeMeshes.Add(Node.id.value(), Marker);
        auto* Label = CreateLabel(TEXT("RouteNodeLabel"), Node.id.value());
        Label->SetText(FText::FromString(UTF8_TO_TCHAR(Node.name.c_str())));
        Label->SetWorldLocation(ToWorld(Node.position_m, 220.0f));
        RouteNodeLabels.Add(Node.id.value(), Label);
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
        Actor->SetActorScale3D(FVector(3.2f, 3.2f, 34.0f));
        Actor->SetActorRotation(FRotator(90.0f, 0.0f, 0.0f));
        Actor->GetStaticMeshComponent()->SetMaterial(0, AircraftMaterial);
        AircraftActors.Add(Aircraft.id.value(), Actor);

        auto* Wings = CreateMirrorActor(TEXT("AircraftWings"), Aircraft.id.value(), CubeMesh);
        Wings->SetActorScale3D(FVector(6.2f, 35.0f, 0.22f));
        Wings->GetStaticMeshComponent()->SetMaterial(0, AircraftMaterial);
        AircraftWingActors.Add(Aircraft.id.value(), Wings);

        TArray<TObjectPtr<AStaticMeshActor>> Details;
        auto AddAircraftPart = [this, &Details, &Aircraft](const FString& Part, UStaticMesh* Mesh, FVector Scale) {
            auto* DetailPart = CreateMirrorActor(Part, Aircraft.id.value(), Mesh);
            DetailPart->SetActorScale3D(Scale);
            DetailPart->GetStaticMeshComponent()->SetMaterial(0, AircraftMaterial);
            Details.Add(DetailPart);
            return DetailPart;
        };
        AddAircraftPart(TEXT("AircraftTailplane"), CubeMesh, FVector(4.5f, 11.5f, 0.18f));
        AddAircraftPart(TEXT("AircraftFin"), ConeMesh, FVector(1.5f, 0.38f, 2.2f));
        AddAircraftPart(TEXT("AircraftEngineL"), CylinderMesh, FVector(0.72f, 0.72f, 4.5f));
        AddAircraftPart(TEXT("AircraftEngineR"), CylinderMesh, FVector(0.72f, 0.72f, 4.5f));
        AddAircraftPart(TEXT("AircraftNose"), ConeMesh, FVector(1.15f, 1.15f, 3.8f));
        AddAircraftPart(TEXT("AircraftTailCone"), ConeMesh, FVector(1.05f, 1.05f, 2.4f));
        for (int32 Gear = 0; Gear < 3; ++Gear) {
            auto* Wheel = AddAircraftPart(TEXT("AircraftLandingWheel"), CylinderMesh, FVector(0.44f, 0.44f, 0.30f));
            Wheel->GetStaticMeshComponent()->SetMaterial(0, AircraftGearMaterial);
        }
        for (int32 Strut = 0; Strut < 3; ++Strut) {
            auto* GearStrut = AddAircraftPart(TEXT("AircraftLandingStrut"), CubeMesh, FVector(0.14f, 0.14f, Strut == 2 ? 3.0f : 3.4f));
            GearStrut->GetStaticMeshComponent()->SetMaterial(0, AircraftGearMaterial);
        }
        AircraftDetailActors.Add(Aircraft.id.value(), MoveTemp(Details));

        auto* Label = CreateLabel(TEXT("AircraftLabel"), Aircraft.id.value());
        Label->SetText(FText::FromString(UTF8_TO_TCHAR(Aircraft.flight_number.c_str())));
        AircraftLabels.Add(Aircraft.id.value(), Label);
    }

    for (const auto& Vehicle : Snapshot.vehicles) {
        if (VehicleActors.Contains(Vehicle.id.value())) continue;
        auto* Actor = CreateMirrorActor(TEXT("Vehicle"), Vehicle.id.value(), CubeMesh);
        const bool bFuel = Vehicle.type == airside::ServiceType::Fueling;
        Actor->SetActorScale3D(bFuel ? FVector(9.0f, 3.1f, 1.0f) : FVector(3.5f, 2.0f, 1.2f));
        Actor->GetStaticMeshComponent()->SetMaterial(0, bFuel ? FuelMaterial : BaggageMaterial);
        VehicleActors.Add(Vehicle.id.value(), Actor);

        auto* Detail = CreateMirrorActor(TEXT("VehicleDetail"), Vehicle.id.value(), bFuel ? CylinderMesh : CubeMesh);
        Detail->SetActorScale3D(bFuel ? FVector(1.45f, 1.45f, 5.2f) : FVector(3.2f, 2.0f, 1.2f));
        if (bFuel) Detail->SetActorRotation(FRotator(90.0f, 0.0f, 0.0f));
        Detail->GetStaticMeshComponent()->SetMaterial(0, bFuel ? AircraftMaterial : BaggageMaterial);
        VehicleDetailActors.Add(Vehicle.id.value(), Detail);

        TArray<TObjectPtr<AStaticMeshActor>> Accessories;
        auto AddVehiclePart = [this, &Accessories, &Vehicle](const FString& Part, UStaticMesh* Mesh, FVector Scale, const FLinearColor& Color) {
            auto* Accessory = CreateMirrorActor(Part, Vehicle.id.value(), Mesh);
            Accessory->SetActorScale3D(Scale);
            Accessory->GetStaticMeshComponent()->SetMaterial(0, CreateMaterial(Color));
            Accessories.Add(Accessory);
            return Accessory;
        };
        AddVehiclePart(TEXT("VehicleCab"), CubeMesh, FVector(2.4f, 2.2f, 1.8f), FLinearColor(0.72f, 0.75f, 0.72f));
        AddVehiclePart(TEXT("VehicleGlass"), CubeMesh, FVector(1.35f, 2.05f, 0.72f), FLinearColor(0.11f, 0.24f, 0.28f));
        for (int32 Wheel = 0; Wheel < 4; ++Wheel)
            AddVehiclePart(TEXT("VehicleWheel"), CylinderMesh, FVector(0.42f, 0.42f, 0.34f), FLinearColor(0.018f, 0.020f, 0.022f));
        if (!bFuel) {
            AddVehiclePart(TEXT("BaggageCartWheelL"), CylinderMesh, FVector(0.38f, 0.38f, 0.30f), FLinearColor(0.018f, 0.020f, 0.022f));
            AddVehiclePart(TEXT("BaggageCartWheelR"), CylinderMesh, FVector(0.38f, 0.38f, 0.30f), FLinearColor(0.018f, 0.020f, 0.022f));
        }
        VehicleAccessoryActors.Add(Vehicle.id.value(), MoveTemp(Accessories));

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
    for (auto& Pair : RouteNodeMeshes) if (Pair.Value) Pair.Value->DestroyComponent();
    for (auto& Pair : RouteNodeLabels) if (Pair.Value) Pair.Value->DestroyComponent();
    for (auto& Pair : ClosureBarriers) if (Pair.Value) Pair.Value->DestroyComponent();
    for (auto& Pair : GateLabels) if (Pair.Value) Pair.Value->DestroyComponent();
    for (auto& Pair : AircraftLabels) if (Pair.Value) Pair.Value->DestroyComponent();
    for (auto& Pair : VehicleLabels) if (Pair.Value) Pair.Value->DestroyComponent();
    for (auto& Pair : GateActors) if (Pair.Value) Pair.Value->Destroy();
    for (auto& Pair : AircraftActors) if (Pair.Value) Pair.Value->Destroy();
    for (auto& Pair : AircraftWingActors) if (Pair.Value) Pair.Value->Destroy();
    for (auto& Pair : AircraftDetailActors) for (auto& Part : Pair.Value) if (Part) Part->Destroy();
    for (auto& Pair : VehicleActors) if (Pair.Value) Pair.Value->Destroy();
    for (auto& Pair : VehicleDetailActors) if (Pair.Value) Pair.Value->Destroy();
    for (auto& Pair : VehicleAccessoryActors) for (auto& Part : Pair.Value) if (Part) Part->Destroy();
    for (auto& Pair : FleetVehicleLabels) if (Pair.Value) Pair.Value->DestroyComponent();
    for (auto& Pair : FleetVehicleActors) if (Pair.Value) Pair.Value->Destroy();
    RoadMeshes.Empty();
    RouteNodeMeshes.Empty();
    RouteNodeLabels.Empty();
    ClosureBarriers.Empty();
    GateLabels.Empty();
    AircraftLabels.Empty();
    VehicleLabels.Empty();
    GateActors.Empty();
    AircraftActors.Empty();
    AircraftWingActors.Empty();
    AircraftDetailActors.Empty();
    VehicleActors.Empty();
    VehicleDetailActors.Empty();
    VehicleAccessoryActors.Empty();
    FleetVehicleLabels.Empty(); FleetVehicleActors.Empty();
    if (AutonomyVehicleActor) AutonomyVehicleActor->Destroy();
    if (GnssMarkerActor) GnssMarkerActor->Destroy();
    for (auto& Actor : AutonomyObstacleActors) if (Actor) Actor->Destroy();
    AutonomyVehicleActor = nullptr;
    GnssMarkerActor = nullptr;
    AutonomyLidarSensor = nullptr;
    AutonomyCameraSensor = nullptr;
    AutonomyObstacleActors.Empty();
    AutonomyTrail.Reset();
    bAutonomyTopologyBuilt = false;
    LastAutonomyTrailSampleTime = -1.0;
}

void ARampLabWorldActor::BuildAutonomyTopology(const airside::autonomy::AutonomySnapshot& Snapshot)
{
    for (auto& Pair : AircraftActors) if (Pair.Value) Pair.Value->SetActorHiddenInGame(true);
    for (auto& Pair : AircraftWingActors) if (Pair.Value) Pair.Value->SetActorHiddenInGame(true);
    for (auto& Pair : VehicleActors) if (Pair.Value) Pair.Value->SetActorHiddenInGame(true);
    for (auto& Pair : VehicleDetailActors) if (Pair.Value) Pair.Value->SetActorHiddenInGame(true);
    for (auto& Pair : AircraftLabels) if (Pair.Value) Pair.Value->SetVisibility(false);
    for (auto& Pair : VehicleLabels) if (Pair.Value) Pair.Value->SetVisibility(false);
    AutonomyVehicleActor = CreateMirrorActor(TEXT("AutonomyTug"), 900001, CubeMesh);
    AutonomyVehicleActor->SetActorScale3D(FVector(6.5f, 3.4f, 1.7f));
    AutonomyVehicleActor->GetStaticMeshComponent()->SetMaterial(0, AutonomyMaterial);
    GnssMarkerActor = CreateMirrorActor(TEXT("GnssEstimate"), 900002, CylinderMesh);
    GnssMarkerActor->SetActorScale3D(FVector(0.35f, 0.35f, 0.35f));
    GnssMarkerActor->GetStaticMeshComponent()->SetMaterial(0, GnssMaterial);
    uint32 Id = 910000;
    for (const auto& Obstacle : Snapshot.obstacles) {
        auto* Actor = CreateMirrorActor(TEXT("AutonomyObstacle"), Id++, CubeMesh);
        Actor->SetActorLocation(ToWorld(Obstacle.center, 55.0f));
        Actor->SetActorScale3D(FVector(static_cast<float>(Obstacle.radius_m*2.0), static_cast<float>(Obstacle.radius_m*2.0), 1.1f));
        Actor->GetStaticMeshComponent()->SetMaterial(0, ObstacleMaterial);
        Actor->GetStaticMeshComponent()->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
        Actor->GetStaticMeshComponent()->SetCollisionResponseToAllChannels(ECR_Ignore);
        Actor->GetStaticMeshComponent()->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
        AutonomyObstacleActors.Add(Actor);
    }
    AutonomyLidarSensor = NewObject<URampLabLidarSensorComponent>(AutonomyVehicleActor, TEXT("RampLabLidar"));
    AutonomyLidarSensor->SetupAttachment(AutonomyVehicleActor->GetRootComponent());
    const auto* Subsystem = GetGameInstance()->GetSubsystem<URampLabSimulationSubsystem>();
    const auto* SensorScenario = Subsystem == nullptr ? nullptr : Subsystem->GetAutonomyScenario();
    const auto LidarExtrinsics = SensorScenario == nullptr
        ? airside::autonomy::SensorExtrinsics{3.40, 0.0, -0.30, 0.0}
        : SensorScenario->sensors.lidar_extrinsics;
    AutonomyLidarSensor->SetRelativeLocation(FVector(LidarExtrinsics.x_m * 100.0, LidarExtrinsics.y_m * 100.0, LidarExtrinsics.z_m * 100.0));
    AutonomyLidarSensor->SetRelativeRotation(FRotator(0.0, FMath::RadiansToDegrees(LidarExtrinsics.yaw_rad), 0.0));
    if (SensorScenario != nullptr) {
        AutonomyLidarSensor->UpdateRateHz = static_cast<float>(SensorScenario->sensors.lidar_hz);
        AutonomyLidarSensor->RayCount = static_cast<int32>(SensorScenario->sensors.lidar_beams);
        AutonomyLidarSensor->HorizontalFovDegrees = FMath::RadiansToDegrees(static_cast<float>(SensorScenario->sensors.lidar_fov_rad));
        AutonomyLidarSensor->MinimumRangeMeters = static_cast<float>(SensorScenario->sensors.lidar_min_range_m);
        AutonomyLidarSensor->MaximumRangeMeters = static_cast<float>(SensorScenario->sensors.lidar_max_range_m);
    }
    AutonomyLidarSensor->RegisterComponent();
    AutonomyCameraSensor = NewObject<URampLabCameraSensorComponent>(AutonomyVehicleActor, TEXT("RampLabCamera"));
    AutonomyCameraSensor->SetupAttachment(AutonomyVehicleActor->GetRootComponent());
    const auto CameraExtrinsics = SensorScenario == nullptr
        ? airside::autonomy::SensorExtrinsics{3.40, 0.0, 0.0, 0.0}
        : SensorScenario->sensors.camera_extrinsics;
    AutonomyCameraSensor->SetRelativeLocation(FVector(CameraExtrinsics.x_m * 100.0, CameraExtrinsics.y_m * 100.0, CameraExtrinsics.z_m * 100.0));
    AutonomyCameraSensor->SetRelativeRotation(FRotator(0.0, FMath::RadiansToDegrees(CameraExtrinsics.yaw_rad), 0.0));
    if (SensorScenario != nullptr) AutonomyCameraSensor->UpdateRateHz = static_cast<float>(SensorScenario->sensors.camera_hz);
    AutonomyCameraSensor->RegisterComponent();
    bAutonomyTopologyBuilt = true;
}

void ARampLabWorldActor::ReconcileAutonomy(const airside::autonomy::AutonomySnapshot& Snapshot)
{
    if (!AutonomyVehicleActor || !GnssMarkerActor) return;
    const auto p = Snapshot.ground_truth.position;
    const FVector Position = ToWorld(p, 100.0f);
    AutonomyVehicleActor->SetActorLocation(Position);
    const FVector Ahead = ToWorld({p.x_m + std::cos(Snapshot.ground_truth.heading_rad), p.y_m + std::sin(Snapshot.ground_truth.heading_rad)}, 100.0f);
    const FVector Direction = Ahead - Position;
    AutonomyVehicleActor->SetActorRotation(FRotator(0.0f, FMath::RadiansToDegrees(FMath::Atan2(Direction.Y, Direction.X)), 0.0f));
    if (AutonomyLidarSensor != nullptr && AutonomyLidarSensor->CaptureAtSimulationTime(Snapshot.timestamp_s)) {
        std::size_t HitCount = 0;
        for (const float Range : AutonomyLidarSensor->RangesMeters) {
            if (Range < AutonomyLidarSensor->MaximumRangeMeters) ++HitCount;
        }
        if (AutonomyLidarSensor->Metadata.Sequence == 1 || HitCount > 0) {
            UE_LOG(LogRampLab, Display, TEXT("Unreal LiDAR frame=%s seq=%llu sim_time=%.2f rays=%d hits=%llu"),
                *AutonomyLidarSensor->Metadata.FrameId,
                static_cast<unsigned long long>(AutonomyLidarSensor->Metadata.Sequence),
                Snapshot.timestamp_s, AutonomyLidarSensor->RangesMeters.Num(),
                static_cast<unsigned long long>(HitCount));
        }
    }
    if (AutonomyCameraSensor != nullptr && AutonomyCameraSensor->CaptureAtSimulationTime(Snapshot.timestamp_s) &&
        AutonomyCameraSensor->Metadata.Sequence == 1) {
        UE_LOG(LogRampLab, Display, TEXT("Unreal RGB camera frame=%s seq=%llu sim_time=%.2f size=%dx%d horizontal_fov=%.1f"),
            *AutonomyCameraSensor->Metadata.FrameId,
            static_cast<unsigned long long>(AutonomyCameraSensor->Metadata.Sequence),
            Snapshot.timestamp_s, AutonomyCameraSensor->ImageWidth, AutonomyCameraSensor->ImageHeight,
            AutonomyCameraSensor->HorizontalFovDegrees);
    }
    if (Snapshot.sensors.gnss) GnssMarkerActor->SetActorLocation(ToWorld(Snapshot.sensors.gnss->position, 150.0f));
    for (std::size_t i=1; i<Snapshot.mission.waypoints.size(); ++i) {
        DrawDebugLine(GetWorld(), ToWorld(Snapshot.mission.waypoints[i-1], 90.0f), ToWorld(Snapshot.mission.waypoints[i], 90.0f), FColor(20, 125, 245), false, 0.0f, 0, 22.0f);
    }
    if (Snapshot.timestamp_s-LastAutonomyTrailSampleTime>=0.2) {
        AutonomyTrail.Add(Position);
        LastAutonomyTrailSampleTime=Snapshot.timestamp_s;
        if (AutonomyTrail.Num()>1200) AutonomyTrail.RemoveAt(0,200,EAllowShrinking::No);
    }
    for (int32 i=1;i<AutonomyTrail.Num();++i) DrawDebugLine(GetWorld(),AutonomyTrail[i-1],AutonomyTrail[i],FColor(255,145,20),false,0.0f,0,8.0f);
    if (AutonomyLidarSensor != nullptr && AutonomyLidarSensor->Metadata.bValid) {
        const auto& Ranges=AutonomyLidarSensor->RangesMeters;
        const std::size_t stride=std::max<std::size_t>(1,static_cast<std::size_t>(Ranges.Num())/60);
        const double AngleMin=FMath::DegreesToRadians(-AutonomyLidarSensor->HorizontalFovDegrees*0.5);
        const double AngleIncrement=-2.0*AngleMin/static_cast<double>(std::max(1,Ranges.Num()-1));
        for(std::size_t i=0;i<static_cast<std::size_t>(Ranges.Num());i+=stride){
            const double Angle=Snapshot.ground_truth.heading_rad+AngleMin+static_cast<double>(i)*AngleIncrement;
            const double Range=Ranges[static_cast<int32>(i)];
            const airside::Vec2 End{p.x_m+Range*std::cos(Angle),p.y_m+Range*std::sin(Angle)};
            DrawDebugLine(GetWorld(),Position,ToWorld(End,100.0f),FColor(255,220,55),false,0.0f,0,2.0f);
        }
    }
}

void ARampLabWorldActor::Reconcile(const airside::SimulationSnapshot& Snapshot, float DeltaSeconds)
{
    for (const auto& Road : Snapshot.roads) {
        if (UStaticMeshComponent* Mesh = RoadMeshes.FindRef(Road.id.value()).Get()) {
            const auto* Subsystem = GetGameInstance()->GetSubsystem<URampLabSimulationSubsystem>();
            UMaterialInstanceDynamic* PermissionMaterial = Road.crosses_runway ? RunwayCrossingRouteMaterial.Get()
                : Road.aircraft_allowed && !Road.ground_vehicle_allowed ? AircraftRouteMaterial.Get()
                : !Road.aircraft_allowed && Road.ground_vehicle_allowed ? GroundVehicleRouteMaterial.Get() : SharedRouteMaterial.Get();
            Mesh->SetMaterial(0, !Road.enabled ? RoadClosedMaterial.Get()
                : (Subsystem != nullptr && Subsystem->IsRoadOnSelectedRoute(Road.id.value()) ? ActiveRouteMaterial.Get() : PermissionMaterial));
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
        TArray<AStaticMeshActor*>& Details = AircraftDetailActors.FindOrAdd(Aircraft.id.value());
        for (auto& DetailPart : Details) if (DetailPart) DetailPart->SetActorHiddenInGame(!bVisible);
        Label->SetVisibility(bVisible);
        if (!bVisible) continue;

        const auto Gate = std::ranges::find(Snapshot.gates, Aircraft.assigned_gate, &airside::GateSnapshot::id);
        if (Gate == Snapshot.gates.end()) continue;
        const airside::Vec2 StandPosition = Aircraft.surface_position_m.value_or(
            airside::Vec2{Gate->position_m.x_m - 14.0, Gate->position_m.y_m});
        const FVector TargetPosition = ToWorld(StandPosition, 430.0f);
        const airside::Vec2 HeadingPoint{
            StandPosition.x_m + std::cos(Aircraft.surface_heading_rad),
            StandPosition.y_m + std::sin(Aircraft.surface_heading_rad)};
        const FVector WorldForward = ToWorld(HeadingPoint) - ToWorld(StandPosition);
        const float WorldYaw = FMath::RadiansToDegrees(FMath::Atan2(WorldForward.Y, WorldForward.X));
        const FRotator PlanRotation(0.0f, WorldYaw, 0.0f);
        const FRotator FuselageTarget(90.0f, WorldYaw, 0.0f);
        const FVector Position = (Aircraft.surface_state == "Taxiing" || Aircraft.surface_state == "TaxiingToGate")
            ? FMath::VInterpTo(Actor->GetActorLocation(), TargetPosition, DeltaSeconds, 4.0f) : TargetPosition;
        const FRotator FuselageRotation = FMath::RInterpTo(Actor->GetActorRotation(), FuselageTarget, DeltaSeconds, 4.0f);
        Actor->SetActorLocation(Position);
        Wings->SetActorLocation(Position - FVector(0.0f, 0.0f, 105.0f));
        Actor->SetActorRotation(FuselageRotation);
        Wings->SetActorRotation(FMath::RInterpTo(Wings->GetActorRotation(), PlanRotation, DeltaSeconds, 4.0f));
        auto SetAircraftPart = [&Details, &Position, DeltaSeconds](int32 Index, FVector Offset, FRotator Rotation) {
            if (Details.IsValidIndex(Index) && Details[Index]) {
                Details[Index]->SetActorLocation(Position + Offset);
                Details[Index]->SetActorRotation(FMath::RInterpTo(Details[Index]->GetActorRotation(), Rotation, DeltaSeconds, 4.0f));
            }
        };
        const FVector TailOffset = PlanRotation.RotateVector(FVector(-1350.0f, 0.0f, 20.0f));
        SetAircraftPart(0, TailOffset + FVector(0.0f, 0.0f, -30.0f), PlanRotation);
        SetAircraftPart(1, TailOffset + FVector(0.0f, 0.0f, 180.0f), PlanRotation);
        const FRotator EngineRotation(90.0f, WorldYaw, 0.0f);
        SetAircraftPart(2, PlanRotation.RotateVector(FVector(170.0f, -570.0f, -65.0f)), EngineRotation);
        SetAircraftPart(3, PlanRotation.RotateVector(FVector(170.0f, 570.0f, -65.0f)), EngineRotation);
        SetAircraftPart(4, PlanRotation.RotateVector(FVector(1580.0f, 0.0f, 0.0f)), EngineRotation);
        SetAircraftPart(5, PlanRotation.RotateVector(FVector(-1580.0f, 0.0f, 0.0f)), FRotator(90.0f, WorldYaw + 180.0f, 0.0f));
        const FVector MainGearLeft(-180.0f, -300.0f, -408.0f);
        const FVector MainGearRight(-180.0f, 300.0f, -408.0f);
        const FVector NoseGear(980.0f, 0.0f, -408.0f);
        SetAircraftPart(6, PlanRotation.RotateVector(MainGearLeft), FRotator(0.0f, WorldYaw, 90.0f));
        SetAircraftPart(7, PlanRotation.RotateVector(MainGearRight), FRotator(0.0f, WorldYaw, 90.0f));
        SetAircraftPart(8, PlanRotation.RotateVector(NoseGear), FRotator(0.0f, WorldYaw, 90.0f));
        SetAircraftPart(9, PlanRotation.RotateVector(FVector(MainGearLeft.X, MainGearLeft.Y, -238.0f)), PlanRotation);
        SetAircraftPart(10, PlanRotation.RotateVector(FVector(MainGearRight.X, MainGearRight.Y, -238.0f)), PlanRotation);
        SetAircraftPart(11, PlanRotation.RotateVector(FVector(NoseGear.X, NoseGear.Y, -265.0f)), PlanRotation);
        Label->SetWorldLocation(Position + FVector(0.0f, 0.0f, 1350.0f));
        const FString SurfaceState = Aircraft.surface_state.empty() ? AircraftStateText(Aircraft.state)
            : UTF8_TO_TCHAR(Aircraft.surface_state.c_str());
        Label->SetText(FText::FromString(FString::Printf(TEXT("%s  /  %s"),
            UTF8_TO_TCHAR(Aircraft.flight_number.c_str()), *SurfaceState)));
        if (Aircraft.surface_route.size() > 1 && FParse::Param(FCommandLine::Get(), TEXT("RampLabShowRoutes"))) {
            const FColor RouteColor = Aircraft.surface_state == "WaitingForTraffic" ? FColor::Yellow : FColor::Cyan;
            for (std::size_t RouteIndex = 1; RouteIndex < Aircraft.surface_route.size(); ++RouteIndex) {
                const auto FromNode = std::ranges::find(Snapshot.road_nodes, Aircraft.surface_route[RouteIndex - 1], &airside::RoadNodeSnapshot::id);
                const auto ToNode = std::ranges::find(Snapshot.road_nodes, Aircraft.surface_route[RouteIndex], &airside::RoadNodeSnapshot::id);
                if (FromNode != Snapshot.road_nodes.end() && ToNode != Snapshot.road_nodes.end())
                    DrawDebugLine(GetWorld(), ToWorld(FromNode->position_m, 20.0f), ToWorld(ToNode->position_m, 20.0f), RouteColor, false, 0.05f, 0, 8.0f);
            }
        }
        UMaterialInstanceDynamic* StateMaterial = Aircraft.state == airside::AircraftState::ReadyForPushback
            ? AircraftReadyMaterial
            : (Aircraft.state == airside::AircraftState::WaitingForServices ? AircraftWaitingMaterial : AircraftMaterial);
        Actor->GetStaticMeshComponent()->SetMaterial(0, StateMaterial);
        Wings->GetStaticMeshComponent()->SetMaterial(0, StateMaterial);
        for (auto& DetailPart : Details) if (DetailPart) DetailPart->GetStaticMeshComponent()->SetMaterial(0, StateMaterial);
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
        if (Vehicle.observed_position_m) {
            PositionMeters = *Vehicle.observed_position_m;
            bFound = true;
        } else if (Vehicle.journey) {
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

        bool bPresentationServicePosition = false;
        TOptional<airside::Vec2> ServiceFacingTarget;
        if (Vehicle.state == airside::VehicleState::Servicing && Vehicle.assigned_aircraft) {
            const auto Aircraft = std::ranges::find(Snapshot.aircraft, *Vehicle.assigned_aircraft, &airside::AircraftSnapshot::id);
            if (Aircraft != Snapshot.aircraft.end()) {
                const auto Gate = std::ranges::find(Snapshot.gates, Aircraft->assigned_gate, &airside::GateSnapshot::id);
                const airside::Vec2 Center = Aircraft->surface_position_m.value_or(
                    Gate == Snapshot.gates.end() ? airside::Vec2{} : Gate->position_m);
                const double Heading = Aircraft->surface_heading_rad;
                const double ForwardX = std::cos(Heading), ForwardY = std::sin(Heading);
                const double SideX = -ForwardY, SideY = ForwardX;
                const double Along = Vehicle.type == airside::ServiceType::Fueling ? 1.0 : -8.0;
                const double Across = Vehicle.type == airside::ServiceType::Fueling ? 9.0 : -7.0;
                PositionMeters = {Center.x_m + ForwardX * Along + SideX * Across,
                    Center.y_m + ForwardY * Along + SideY * Across};
                ServiceFacingTarget = Center;
                bPresentationServicePosition = true;
            }
        }

        const double Heading = Vehicle.observed_heading_rad.value_or(0.0);
        if (bPresentationServicePosition && ServiceFacingTarget) {
            TargetYaw = FMath::RadiansToDegrees(FMath::Atan2(
                ToWorld(ServiceFacingTarget.GetValue()).Y - ToWorld(PositionMeters).Y,
                ToWorld(ServiceFacingTarget.GetValue()).X - ToWorld(PositionMeters).X));
        }
        const FVector VehicleForward = ToWorld({PositionMeters.x_m + std::cos(Heading), PositionMeters.y_m + std::sin(Heading)})
            - ToWorld(PositionMeters);
        if (!bPresentationServicePosition && (!Vehicle.journey || Vehicle.observed_heading_rad))
            TargetYaw = FMath::RadiansToDegrees(FMath::Atan2(VehicleForward.Y, VehicleForward.X));

        const FVector Position = ToWorld(PositionMeters, 85.0f);
        const FVector RenderPosition = Actor->GetActorLocation().IsNearlyZero()
            ? Position : FMath::VInterpTo(Actor->GetActorLocation(), Position, DeltaSeconds, 5.0f);
        Actor->SetActorLocation(RenderPosition);
        FRotator Smooth = Actor->GetActorRotation();
        if (TargetYaw) {
            Smooth = FMath::RInterpTo(Smooth, FRotator(0.0f, *TargetYaw, 0.0f), DeltaSeconds, 5.0f);
            Actor->SetActorRotation(Smooth);
        }
        const bool bFuelVehicle = Vehicle.type == airside::ServiceType::Fueling;
        const FVector RearOrTank = bFuelVehicle ? RenderPosition + FVector(0.0f, 0.0f, 135.0f)
            : RenderPosition + Smooth.RotateVector(FVector(-360.0f, 0.0f, 10.0f));
        Detail->SetActorLocation(RearOrTank);
        Detail->SetActorRotation(bFuelVehicle ? FRotator(90.0f, Smooth.Yaw, 0.0f) : Smooth);
        TArray<AStaticMeshActor*>& Accessories = VehicleAccessoryActors.FindOrAdd(Vehicle.id.value());
        const FVector Cab = RenderPosition + Smooth.RotateVector(FVector(90.0f, 0.0f, 105.0f));
        const FVector Glass = RenderPosition + Smooth.RotateVector(FVector(165.0f, 0.0f, 130.0f));
        if (Accessories.IsValidIndex(0) && Accessories[0]) { Accessories[0]->SetActorLocation(Cab); Accessories[0]->SetActorRotation(Smooth); }
        if (Accessories.IsValidIndex(1) && Accessories[1]) { Accessories[1]->SetActorLocation(Glass); Accessories[1]->SetActorRotation(Smooth); }
        for (int32 Wheel = 0; Wheel < 4; ++Wheel) {
            if (!Accessories.IsValidIndex(Wheel + 2) || !Accessories[Wheel + 2]) continue;
            const float Longitudinal = Wheel < 2 ? 215.0f : -205.0f;
            const float Lateral = (Wheel % 2 == 0 ? -1.0f : 1.0f) * 120.0f;
            Accessories[Wheel + 2]->SetActorLocation(RenderPosition + Smooth.RotateVector(FVector(Longitudinal, Lateral, -64.0f)));
            Accessories[Wheel + 2]->SetActorRotation(FRotator(90.0f, Smooth.Yaw, 0.0f));
        }
        if (!bFuelVehicle) for (int32 Wheel = 0; Wheel < 2; ++Wheel) {
            const int32 Index = Wheel + 6;
            if (!Accessories.IsValidIndex(Index) || !Accessories[Index]) continue;
            const float Side = Wheel == 0 ? -85.0f : 85.0f;
            Accessories[Index]->SetActorLocation(RenderPosition + Smooth.RotateVector(FVector(-490.0f, Side, -60.0f)));
            Accessories[Index]->SetActorRotation(FRotator(90.0f, Smooth.Yaw, 0.0f));
        }
        Label->SetWorldLocation(RenderPosition + FVector(0.0f, 0.0f, 240.0f));
        Label->SetText(FText::FromString(FString::Printf(
            TEXT("%s - %s"), UTF8_TO_TCHAR(Vehicle.name.c_str()),
            Vehicle.fleet_status.empty() ? *VehicleStateText(Vehicle.state) : UTF8_TO_TCHAR(Vehicle.fleet_status.c_str()))));
    }

    const FVector CameraLocation = CameraComponent->GetComponentLocation();
    for (const auto& Pair : RouteNodeLabels) if (Pair.Value) Pair.Value->SetWorldRotation((CameraLocation - Pair.Value->GetComponentLocation()).Rotation());
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
    Parameters.Name = *FString::Printf(TEXT("%s_%u_G%u_A%u"), *Prefix, Id, TopologyGeneration, ++MirrorActorSerial);
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

    const auto* AutonomySnapshot = Subsystem->GetAutonomySnapshot();
    const double Time = AutonomySnapshot != nullptr
        ? AutonomySnapshot->timestamp_s
        : static_cast<double>(Subsystem->GetPlaybackTime().count());
    if (Subsystem->GetScenarioName().Equals(TEXT("goal10_sensor_validation"), ESearchCase::IgnoreCase)) {
        if (CaptureStage >= 2 || Time < (CaptureStage == 0 ? 0.0 : 5.0)) return;
        Subsystem->SetCameraPreset(TEXT("Service Roads"));
        ApplyCameraPreset(TEXT("Service Roads"));
        if (const auto* CaptureSnapshot = Subsystem->GetSnapshot()) Reconcile(*CaptureSnapshot, 0.0f);
        const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Screenshots"), TEXT("RampLab"));
        IFileManager::Get().MakeDirectory(*Directory, true);
        const FString Name = CaptureStage == 0 ? TEXT("goal10-sensor-validation-start.png") : TEXT("goal10-sensor-validation-obstacle.png");
        const FString Filename = FPaths::Combine(Directory, Name);
        FScreenshotRequest::RequestScreenshot(Filename, true, false, false, FIntRect(), true);
        UE_LOG(LogRampLab, Display, TEXT("Goal 10 Unreal sensor capture saved: %s simulation_time=%.2f"), *Filename, Time);
        ++CaptureStage;
        if (CaptureStage == 2) CaptureExitCountdown = 2.0f;
        return;
    }
    struct FCapturePoint { const TCHAR* Scenario; int64 Time; const TCHAR* Name; const TCHAR* Camera; };
    const FCapturePoint Points[] = {
        {TEXT("baseline"), 0, TEXT("00-auburn-overview"), TEXT("Overview")},
        {TEXT("baseline"), 240, TEXT("01-baseline-operations"), TEXT("Ramp")},
        {TEXT("baseline"), 305, TEXT("02-road-closure"), TEXT("Service Roads")},
        {TEXT("baseline"), 900, TEXT("03-alternate-route"), TEXT("Service Roads")},
        {TEXT("baseline"), 3000, TEXT("04-baseline-results"), TEXT("Ramp")},
        {TEXT("high_capacity"), 2100, TEXT("05-high-capacity-comparison"), TEXT("Overview")},
        {TEXT("autonomy_tug"), 0, TEXT("06-autonomy-depot-route"), TEXT("Service Roads")},
        {TEXT("autonomy_tug"), 20, TEXT("07-autonomy-sensors-obstacles"), TEXT("Service Roads")},
        {TEXT("autonomy_tug"), 66, TEXT("08-autonomy-gate-a2-result"), TEXT("Gate A2")},
    };
    if (CaptureStage >= UE_ARRAY_COUNT(Points)) return;
    const auto& Point = Points[CaptureStage];
    const bool bFinalAutonomyCapture = CaptureStage == UE_ARRAY_COUNT(Points) - 1;
    if (!Subsystem->GetScenarioName().Equals(Point.Scenario, ESearchCase::IgnoreCase)
        || Time < static_cast<double>(Point.Time)
        || (bFinalAutonomyCapture && !Subsystem->IsFinished())) return;
    Subsystem->SetCameraPreset(Point.Camera);
    ApplyCameraPreset(Point.Camera);
    if (const auto* CaptureSnapshot = Subsystem->GetSnapshot()) Reconcile(*CaptureSnapshot, 0.0f);

    const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Screenshots"), TEXT("RampLab"));
    IFileManager::Get().MakeDirectory(*Directory, true);
    const FString Filename = FPaths::Combine(Directory, FString(Point.Name) + TEXT(".png"));
    FScreenshotRequest::RequestScreenshot(Filename, true, false, false, FIntRect(), true);
    UE_LOG(LogRampLab, Display, TEXT("RampLab capture saved: %s scenario=%s time=%.2f"),
        *Filename, *Subsystem->GetScenarioName(), Time);
    ++CaptureStage;
    if (CaptureStage == UE_ARRAY_COUNT(Points)) CaptureExitCountdown = 2.0f;
}

FVector ARampLabWorldActor::ToWorld(airside::Vec2 Meters, float HeightCm) const
{
    const auto* Subsystem = GetGameInstance()->GetSubsystem<URampLabSimulationSubsystem>();
    if (IsDemoAirport(Subsystem))
        return FVector(Meters.x_m * 100.0, -Meters.y_m * 100.0, HeightCm);
    return Placement.ToUnreal(Meters, HeightCm);
}

void ARampLabWorldActor::UpdateCamera(float DeltaSeconds)
{
    const auto* Subsystem = GetGameInstance() == nullptr ? nullptr : GetGameInstance()->GetSubsystem<URampLabSimulationSubsystem>();
    if (Subsystem != nullptr && AppliedCameraPreset != Subsystem->GetCameraPreset()) ApplyCameraPreset(Subsystem->GetCameraPreset());

    if (Subsystem != nullptr && (AppliedCameraPreset == TEXT("Follow Aircraft") || AppliedCameraPreset == TEXT("Follow Vehicle"))) {
        const auto* Snapshot = Subsystem->GetSnapshot();
        if (Snapshot != nullptr && AppliedCameraPreset == TEXT("Follow Aircraft")) {
            auto Aircraft = std::ranges::find(Snapshot->aircraft, Subsystem->GetSelectedEntityId(),
                [](const auto& Item) { return Item.id.value(); });
            if (Aircraft == Snapshot->aircraft.end() || Aircraft->state == airside::AircraftState::Scheduled
                || Aircraft->state == airside::AircraftState::Departed)
                Aircraft = std::ranges::find_if(Snapshot->aircraft, [](const auto& Item) {
                    return Item.state != airside::AircraftState::Scheduled && Item.state != airside::AircraftState::Departed;
                });
            if (Aircraft != Snapshot->aircraft.end()) {
                const auto Gate = std::ranges::find(Snapshot->gates, Aircraft->assigned_gate, &airside::GateSnapshot::id);
                const auto Position = Aircraft->surface_position_m.value_or(Gate == Snapshot->gates.end() ? airside::Vec2{} : Gate->position_m);
                CameraFocus = FMath::VInterpTo(CameraFocus, ToWorld(Position, 180.0f), DeltaSeconds, 3.0f);
            }
        } else if (Snapshot != nullptr && AppliedCameraPreset == TEXT("Follow Vehicle")) {
            auto Vehicle = std::ranges::find(Snapshot->vehicles, Subsystem->GetSelectedEntityId(),
                [](const auto& Item) { return Item.id.value(); });
            if (Vehicle == Snapshot->vehicles.end() || !Vehicle->observed_position_m)
                Vehicle = std::ranges::find_if(Snapshot->vehicles, [](const auto& Item) { return Item.observed_position_m.has_value(); });
            if (Vehicle != Snapshot->vehicles.end())
                CameraFocus = FMath::VInterpTo(CameraFocus, ToWorld(*Vehicle->observed_position_m, 160.0f), DeltaSeconds, 3.0f);
        }
    }

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
        CameraDistance = FMath::Clamp(CameraDistance - Controller->GetInputAnalogKeyState(EKeys::MouseWheelAxis) * 5000.0f, 12000.0f, 260000.0f);
    }
    const FRotator Rotation(CameraPitch, CameraYaw, 0.0f);
    CameraComponent->SetWorldRotation(Rotation);
    CameraComponent->SetWorldLocation(CameraFocus - Rotation.Vector() * CameraDistance);
}

void ARampLabWorldActor::ApplyCameraPreset(const FString& Preset)
{
    AppliedCameraPreset = Preset;
    const auto* Subsystem = GetGameInstance()->GetSubsystem<URampLabSimulationSubsystem>();
    const bool bDemoAirport = IsDemoAirport(Subsystem);
    if (bDemoAirport && (Preset == TEXT("Airport Overview") || Preset == TEXT("Overview") || Preset == TEXT("KAUO Overview"))) {
        CameraFocus = ToWorld({0.0, 150.0}, 0.0f); CameraDistance = 184000.0f; CameraYaw = -36.0f; CameraPitch = -66.0f;
    } else if (bDemoAirport && Preset == TEXT("Apron")) {
        CameraFocus = ToWorld({0.0, 380.0}, 0.0f); CameraDistance = 100000.0f; CameraYaw = -27.0f; CameraPitch = -57.0f;
    } else if (bDemoAirport && (Preset == TEXT("Ramp") || Preset == TEXT("Ramp Level"))) {
        CameraFocus = ToWorld({300.0, 270.0}, 350.0f); CameraDistance = 35000.0f; CameraYaw = -22.0f; CameraPitch = -53.0f;
    } else if (bDemoAirport && Preset == TEXT("Runway")) {
        CameraFocus = ToWorld({0.0, 15.0}, 0.0f); CameraDistance = 154000.0f; CameraYaw = -90.0f; CameraPitch = -76.0f;
    } else if (bDemoAirport && Preset == TEXT("Gate A2")) {
        CameraFocus = ToWorld({-200.0, 270.0}, 0.0f); CameraDistance = 26000.0f; CameraYaw = -30.0f; CameraPitch = -42.0f;
    } else if (bDemoAirport && Preset == TEXT("Follow Aircraft")) {
        CameraFocus = ToWorld({200.0, 270.0}, 430.0f); CameraDistance = 20000.0f; CameraYaw = -35.0f; CameraPitch = -48.0f;
    } else if (bDemoAirport && Preset == TEXT("Follow Vehicle")) {
        CameraFocus = ToWorld({0.0, 430.0}, 160.0f); CameraDistance = 24000.0f; CameraYaw = -35.0f; CameraPitch = -48.0f;
    } else if (Preset == TEXT("Overview") || Preset == TEXT("KAUO Overview") || Preset == TEXT("KAUOOverview")) {
        CameraFocus = ToWorld({0.0, 10.0}); CameraDistance = 220000.0f; CameraYaw = -42.0f; CameraPitch = -68.0f;
    } else if (Preset == TEXT("Runway") || Preset == TEXT("KAUORunways")) {
        CameraFocus = ToWorld({0.0, 0.0}); CameraDistance = 105000.0f; CameraYaw = 0.0f; CameraPitch = -88.0f;
    } else if (Preset == TEXT("Apron") || Preset == TEXT("KAUOApron")) {
        CameraFocus = ToWorld({115.0, -138.0}); CameraDistance = 29000.0f; CameraYaw = -38.0f; CameraPitch = -52.0f;
    } else if (Preset == TEXT("KAUOIntersection")) {
        CameraFocus = ToWorld({-193.0, 458.0}); CameraDistance = 48000.0f; CameraYaw = 0.0f; CameraPitch = -86.0f;
    } else if (Preset == TEXT("Ramp")) {
        CameraFocus = ToWorld({120.0, 0.0}); CameraDistance = 40000.0f; CameraYaw = -42.0f; CameraPitch = -55.0f;
    } else if (Preset == TEXT("Gate A2")) {
        CameraFocus = ToWorld({240.0, 0.0}); CameraDistance = 18500.0f; CameraYaw = -60.0f; CameraPitch = -43.0f;
    } else if (Preset == TEXT("Service Roads")) {
        CameraFocus = ToWorld({120.0, 0.0}); CameraDistance = 36000.0f; CameraYaw = -18.0f; CameraPitch = -62.0f;
    } else if (Preset == TEXT("Follow Aircraft")) {
        CameraFocus = ToWorld({115.0, -138.0}, 180.0f); CameraDistance = 15000.0f; CameraYaw = -50.0f; CameraPitch = -30.0f;
    } else if (Preset == TEXT("Follow Vehicle")) {
        CameraFocus = ToWorld({115.0, -138.0}, 160.0f); CameraDistance = 9000.0f; CameraYaw = -35.0f; CameraPitch = -25.0f;
    } else {
        CameraFocus = FVector(15000.0f, 0.0f, 0.0f); CameraDistance = 82000.0f; CameraYaw = -38.0f; CameraPitch = -54.0f;
    }
    const FRotator Rotation(CameraPitch, CameraYaw, 0.0f);
    CameraComponent->SetWorldRotation(Rotation);
    CameraComponent->SetWorldLocation(CameraFocus - Rotation.Vector() * CameraDistance);
}
