#include "RampLabDemoGameMode.h"

#include "RampLabSimulationSubsystem.h"
#include "RampLabWorldActor.h"

#include "Components/DirectionalLightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Engine/DirectionalLight.h"
#include "Engine/SkyLight.h"
#include "GameFramework/PlayerController.h"

ARampLabDemoGameMode::ARampLabDemoGameMode()
{
    DefaultPawnClass = nullptr;
}

void ARampLabDemoGameMode::BeginPlay()
{
    Super::BeginPlay();

    auto* Viewer = GetWorld()->SpawnActor<ARampLabWorldActor>();
    if (auto* Sun = GetWorld()->SpawnActor<ADirectionalLight>(FVector::ZeroVector, FRotator(-55.0f, -35.0f, 0.0f))) {
        Sun->GetLightComponent()->SetIntensity(8.0f);
    }
    if (auto* Sky = GetWorld()->SpawnActor<ASkyLight>()) {
        Sky->GetLightComponent()->SetIntensity(1.5f);
    }
    if (auto* Subsystem = GetGameInstance()->GetSubsystem<URampLabSimulationSubsystem>()) {
        Subsystem->AttachControlPanel();
    }

    if (auto* Controller = GetWorld()->GetFirstPlayerController()) {
        Controller->SetViewTarget(Viewer);
        Controller->bShowMouseCursor = true;
        Controller->SetInputMode(FInputModeGameAndUI{});
    }
}
