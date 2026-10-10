#include "RampLabDemoGameMode.h"

#include "RampLabSimulationSubsystem.h"
#include "RampLabAirportEnvironment.h"
#include "RampLabWorldActor.h"

#include "Components/DirectionalLightComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Engine/DirectionalLight.h"
#include "Engine/SkyLight.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

ARampLabDemoGameMode::ARampLabDemoGameMode()
{
    DefaultPawnClass = nullptr;
}

void ARampLabDemoGameMode::BeginPlay()
{
    Super::BeginPlay();

    // A Cesium globe routinely exceeds Unreal's conventional world bounds.
    GetWorld()->GetWorldSettings()->bEnableWorldBoundsChecks = false;

    auto* Viewer = GetWorld()->SpawnActor<ARampLabWorldActor>();
    GetWorld()->SpawnActor<ARampLabAirportEnvironment>();
    GetWorld()->SpawnActor<ASkyAtmosphere>();
    if (auto* Sun = GetWorld()->SpawnActor<ADirectionalLight>(FVector::ZeroVector, FRotator(-55.0f, -35.0f, 0.0f))) {
        auto* SunComponent = CastChecked<UDirectionalLightComponent>(Sun->GetLightComponent());
        SunComponent->SetMobility(EComponentMobility::Movable);
        SunComponent->SetIntensity(3.0f);
        SunComponent->SetCastShadows(true);
        SunComponent->SetAtmosphereSunLight(true);
    }
    if (auto* Sky = GetWorld()->SpawnActor<ASkyLight>()) {
        Sky->GetLightComponent()->SetMobility(EComponentMobility::Movable);
        Sky->GetLightComponent()->SetIntensity(1.1f);
        Sky->GetLightComponent()->SetRealTimeCapture(true);
    }
    if (auto* Subsystem = GetGameInstance()->GetSubsystem<URampLabSimulationSubsystem>()) {
        if (!FParse::Param(FCommandLine::Get(), TEXT("RampLabHideControlPanel")))
            Subsystem->AttachControlPanel();
    }

    if (auto* Controller = GetWorld()->GetFirstPlayerController()) {
        Controller->SetViewTarget(Viewer);
        Controller->bShowMouseCursor = true;
        Controller->SetInputMode(FInputModeGameAndUI{});
    }
}
