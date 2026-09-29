#pragma once

#include "CoreMinimal.h"
#include "Components/SceneCaptureComponent2D.h"

#include "RampLabSensors.generated.h"

USTRUCT()
struct FRampLabSensorFrameMetadata
{
    GENERATED_BODY()
    UPROPERTY() double TimestampSeconds{0.0};
    UPROPERTY() uint64 Sequence{0};
    UPROPERTY() FString SensorId;
    UPROPERTY() FString FrameId;
    UPROPERTY() bool bValid{false};
};

UCLASS(ClassGroup=(RampLab), meta=(BlueprintSpawnableComponent))
class RAMPLABINTEGRATION_API URampLabLidarSensorComponent final : public USceneComponent
{
    GENERATED_BODY()
public:
    URampLabLidarSensorComponent();
    bool CaptureAtSimulationTime(double TimeSeconds);
    UPROPERTY(EditAnywhere, Category="Sensor", meta=(ClampMin="1")) int32 RayCount{181};
    UPROPERTY(EditAnywhere, Category="Sensor", meta=(ClampMin="1.0", ClampMax="360.0")) float HorizontalFovDegrees{180.0f};
    UPROPERTY(EditAnywhere, Category="Sensor", meta=(ClampMin="0.01")) float MinimumRangeMeters{0.1f};
    UPROPERTY(EditAnywhere, Category="Sensor", meta=(ClampMin="0.1")) float MaximumRangeMeters{30.0f};
    UPROPERTY(VisibleAnywhere, Category="Sensor") FRampLabSensorFrameMetadata Metadata;
    UPROPERTY(VisibleAnywhere, Category="Sensor") TArray<float> RangesMeters;
private:
    double NextCaptureSeconds{0.0};
};

UCLASS(ClassGroup=(RampLab), meta=(BlueprintSpawnableComponent))
class RAMPLABINTEGRATION_API URampLabCameraSensorComponent final : public USceneCaptureComponent2D
{
    GENERATED_BODY()
public:
    URampLabCameraSensorComponent();
    bool CaptureAtSimulationTime(double TimeSeconds);
    UPROPERTY(EditAnywhere, Category="Sensor", meta=(ClampMin="1.0", ClampMax="179.0")) float HorizontalFovDegrees{90.0f};
    UPROPERTY(EditAnywhere, Category="Sensor", meta=(ClampMin="1")) int32 ImageWidth{320};
    UPROPERTY(EditAnywhere, Category="Sensor", meta=(ClampMin="1")) int32 ImageHeight{180};
    UPROPERTY(VisibleAnywhere, Category="Sensor") FRampLabSensorFrameMetadata Metadata;
private:
    TObjectPtr<class UTextureRenderTarget2D> ImageTarget;
    double NextCaptureSeconds{0.0};
};
