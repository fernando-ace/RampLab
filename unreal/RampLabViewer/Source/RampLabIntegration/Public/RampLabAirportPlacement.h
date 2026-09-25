#pragma once

#include "CoreMinimal.h"

namespace airside { struct Vec2; }

struct RAMPLABINTEGRATION_API FRampLabAirportPlacement
{
    double Latitude{32.6151667};
    double Longitude{-85.4340000};
    double HeightMeters{208.27};
    double SimulationHeadingDegrees{95.7};
    FVector2D OriginOffsetMeters{80.0, 0.0};
    double Scale{1.0};
    double OperationalLayerHeightCm{30.0};
    int64 TerrainAssetId{1};
    int64 ImageryAssetId{2};

    static FRampLabAirportPlacement Load();
    [[nodiscard]] FVector ToUnreal(airside::Vec2 LocalMeters, float AdditionalHeightCm = 0.0f) const;
};
