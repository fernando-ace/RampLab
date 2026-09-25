#include "SRampLabControlPanel.h"

#include "RampLabSimulationSubsystem.h"
#include "airside/agents/aircraft.hpp"
#include "airside/agents/service_vehicle.hpp"

#include "Styling/AppStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

void SRampLabControlPanel::Construct(const FArguments& Arguments)
{
    SimulationSubsystem = Arguments._Subsystem;

    ChildSlot
    [
        SNew(SBox)
        .WidthOverride(390.0f)
        .Padding(18.0f)
        [
            SNew(SBorder)
            .BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
            .Padding(14.0f)
            [
                SNew(SVerticalBox)
                + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
                [
                    SNew(STextBlock)
                    .Text(FText::FromString(TEXT("RampLab")))
                    .Font(FAppStyle::GetFontStyle("HeadingExtraSmall"))
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 10)
                [
                    SNew(STextBlock).Text(this, &SRampLabControlPanel::SummaryText)
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
                    [ SNew(SButton).Text(FText::FromString(TEXT("Play / Pause"))).OnClicked(this, &SRampLabControlPanel::TogglePlay) ]
                    + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 12, 0)
                    [ SNew(SButton).Text(FText::FromString(TEXT("Reset"))).OnClicked(this, &SRampLabControlPanel::Reset) ]
                    + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
                    [ SNew(SButton).Text(FText::FromString(TEXT("1x"))).OnClicked(this, &SRampLabControlPanel::SetSpeed, 1.0) ]
                    + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
                    [ SNew(SButton).Text(FText::FromString(TEXT("5x"))).OnClicked(this, &SRampLabControlPanel::SetSpeed, 5.0) ]
                    + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
                    [ SNew(SButton).Text(FText::FromString(TEXT("10x"))).OnClicked(this, &SRampLabControlPanel::SetSpeed, 10.0) ]
                    + SHorizontalBox::Slot().AutoWidth()
                    [ SNew(SButton).Text(FText::FromString(TEXT("20x"))).OnClicked(this, &SRampLabControlPanel::SetSpeed, 20.0) ]
                ]
                + SVerticalBox::Slot().AutoHeight()
                [
                    SNew(STextBlock)
                    .Text(this, &SRampLabControlPanel::EventsText)
                    .AutoWrapText(true)
                ]
            ]
        ]
    ];
}

FText SRampLabControlPanel::SummaryText() const
{
    const auto* Subsystem = SimulationSubsystem.Get();
    if (Subsystem == nullptr || !Subsystem->IsReady()) {
        return FText::FromString(Subsystem == nullptr ? TEXT("Simulation unavailable") : Subsystem->GetStatusText());
    }

    const auto Time = Subsystem->GetPlaybackTime().count();
    const auto* Snapshot = Subsystem->GetSnapshot();
    int32 ActiveAircraft = 0;
    int32 FuelTrucks = 0;
    int32 BaggageCarts = 0;
    if (Snapshot != nullptr) {
        for (const auto& Aircraft : Snapshot->aircraft) {
            if (Aircraft.state != airside::AircraftState::Departed) ++ActiveAircraft;
        }
        for (const auto& Vehicle : Snapshot->vehicles) {
            Vehicle.type == airside::ServiceType::Fueling ? ++FuelTrucks : ++BaggageCarts;
        }
    }

    return FText::FromString(FString::Printf(
        TEXT("Simulation Time: %02lld:%02lld:%02lld\nSpeed: %.0fx   State: %s\nAircraft: %d active\nFuel Trucks: %d   Baggage Carts: %d"),
        Time / 3600, (Time / 60) % 60, Time % 60,
        Subsystem->GetPlaybackSpeed(),
        Subsystem->IsFinished() ? TEXT("Finished") : (Subsystem->IsPlaying() ? TEXT("Playing") : TEXT("Paused")),
        ActiveAircraft, FuelTrucks, BaggageCarts));
}

FText SRampLabControlPanel::EventsText() const
{
    const auto* Subsystem = SimulationSubsystem.Get();
    if (Subsystem == nullptr) return FText::GetEmpty();
    FString Result(TEXT("\nRecent events\n"));
    for (const auto& Event : Subsystem->GetRecentEvents()) Result += Event + TEXT("\n");
    return FText::FromString(Result);
}

FReply SRampLabControlPanel::TogglePlay()
{
    if (auto* Subsystem = SimulationSubsystem.Get()) Subsystem->TogglePlaying();
    return FReply::Handled();
}

FReply SRampLabControlPanel::Reset()
{
    if (auto* Subsystem = SimulationSubsystem.Get()) Subsystem->ResetSimulation();
    return FReply::Handled();
}

FReply SRampLabControlPanel::SetSpeed(double Speed)
{
    if (auto* Subsystem = SimulationSubsystem.Get()) Subsystem->SetPlaybackSpeed(Speed);
    return FReply::Handled();
}
