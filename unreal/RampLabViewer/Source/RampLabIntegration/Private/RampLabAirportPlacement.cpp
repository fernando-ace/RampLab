#include "RampLabAirportPlacement.h"

#include "airside/world/airport_graph.hpp"

#include "Misc/ConfigCacheIni.h"

namespace {
constexpr TCHAR Section[] = TEXT("RampLab.AirportPlacement");
}

FRampLabAirportPlacement FRampLabAirportPlacement::Load()
{
    FRampLabAirportPlacement Result;
    if (GConfig == nullptr) return Result;

    GConfig->GetDouble(Section, TEXT("Latitude"), Result.Latitude, GGameIni);
    GConfig->GetDouble(Section, TEXT("Longitude"), Result.Longitude, GGameIni);
    GConfig->GetDouble(Section, TEXT("HeightMeters"), Result.HeightMeters, GGameIni);
    GConfig->GetDouble(Section, TEXT("SimulationHeadingDegrees"), Result.SimulationHeadingDegrees, GGameIni);
    GConfig->GetDouble(Section, TEXT("OriginOffsetEastMeters"), Result.OriginOffsetMeters.X, GGameIni);
    GConfig->GetDouble(Section, TEXT("OriginOffsetNorthMeters"), Result.OriginOffsetMeters.Y, GGameIni);
    GConfig->GetDouble(Section, TEXT("Scale"), Result.Scale, GGameIni);
    GConfig->GetDouble(Section, TEXT("OperationalLayerHeightCm"), Result.OperationalLayerHeightCm, GGameIni);
    GConfig->GetInt64(Section, TEXT("TerrainAssetId"), Result.TerrainAssetId, GGameIni);
    GConfig->GetInt64(Section, TEXT("ImageryAssetId"), Result.ImageryAssetId, GGameIni);
    return Result;
}

FVector FRampLabAirportPlacement::ToUnreal(airside::Vec2 LocalMeters, float AdditionalHeightCm) const
{
    const FVector2D Scaled{LocalMeters.x_m * Scale, LocalMeters.y_m * Scale};
    const double Radians = FMath::DegreesToRadians(SimulationHeadingDegrees);
    // Simulation Vec2 is right-handed east/north. Rotate it by the configured
    // clockwise-from-true-north bearing of local +X, then map north to the
    // viewer's south-positive Unreal Y axis.
    const double East = Scaled.X * FMath::Sin(Radians) - Scaled.Y * FMath::Cos(Radians) + OriginOffsetMeters.X;
    const double North = Scaled.X * FMath::Cos(Radians) + Scaled.Y * FMath::Sin(Radians) + OriginOffsetMeters.Y;
    return FVector(East * 100.0, -North * 100.0, OperationalLayerHeightCm + AdditionalHeightCm);
}
