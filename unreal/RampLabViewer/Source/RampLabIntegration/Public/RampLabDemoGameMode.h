#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"

#include "RampLabDemoGameMode.generated.h"

UCLASS()
class RAMPLABINTEGRATION_API ARampLabDemoGameMode final : public AGameModeBase
{
    GENERATED_BODY()

public:
    ARampLabDemoGameMode();
    virtual void BeginPlay() override;
};
