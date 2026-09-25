#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class URampLabSimulationSubsystem;

class SRampLabControlPanel final : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SRampLabControlPanel) {}
        SLATE_ARGUMENT(TWeakObjectPtr<URampLabSimulationSubsystem>, Subsystem)
    SLATE_END_ARGS()

    void Construct(const FArguments& Arguments);

private:
    FText SummaryText() const;
    FText EventsText() const;
    FText SelectedEntityText() const;
    FText ResultsText() const;
    FReply TogglePlay();
    FReply Reset();
    FReply SetSpeed(double Speed);
    FReply SelectScenario(FString Scenario);
    FReply SelectEntity(FString Kind);
    FReply SetCamera(FString Preset);

    TWeakObjectPtr<URampLabSimulationSubsystem> SimulationSubsystem;
};
