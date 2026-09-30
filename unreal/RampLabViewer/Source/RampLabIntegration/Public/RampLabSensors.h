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
    UPROPERTY(VisibleAnywhere) double CaptureCostMilliseconds{0.0};
};

UCLASS(ClassGroup=(RampLab), meta=(BlueprintSpawnableComponent))
class RAMPLABINTEGRATION_API URampLabLidarSensorComponent final : public USceneComponent
{
    GENERATED_BODY()
public:
    URampLabLidarSensorComponent();
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    bool CaptureAtSimulationTime(double TimeSeconds);
    [[nodiscard]] uint64 GetMeasuredCaptureCount() const noexcept { return MeasuredCaptureCount; }
    [[nodiscard]] uint64 GetTotalHitReturns() const noexcept { return TotalHitReturns; }
    [[nodiscard]] double GetMeanCaptureCostMilliseconds() const noexcept {
        return MeasuredCaptureCount == 0 ? 0.0 : AccumulatedCaptureCostMilliseconds / static_cast<double>(MeasuredCaptureCount);
    }
    UPROPERTY(EditAnywhere, Category="Sensor", meta=(ClampMin="1")) int32 RayCount{181};
    UPROPERTY(EditAnywhere, Category="Sensor", meta=(ClampMin="0.1")) float UpdateRateHz{10.0f};
    UPROPERTY(EditAnywhere, Category="Sensor", meta=(ClampMin="1", ClampMax="65535")) int32 TransportPort{39011};
    UPROPERTY(EditAnywhere, Category="Sensor", meta=(ClampMin="1.0", ClampMax="360.0")) float HorizontalFovDegrees{180.0f};
    UPROPERTY(EditAnywhere, Category="Sensor", meta=(ClampMin="0.01")) float MinimumRangeMeters{0.1f};
    UPROPERTY(EditAnywhere, Category="Sensor", meta=(ClampMin="0.1")) float MaximumRangeMeters{30.0f};
    UPROPERTY(VisibleAnywhere, Category="Sensor") FRampLabSensorFrameMetadata Metadata;
    UPROPERTY(VisibleAnywhere, Category="Sensor") TArray<float> RangesMeters;
private:
    double NextCaptureSeconds{0.0};
    double AccumulatedCaptureCostMilliseconds{0.0};
    uint64 MeasuredCaptureCount{0};
    uint64 TotalHitReturns{0};
    class FSocket* TransportSocket{};
    double NextConnectAttemptSeconds{0.0};
    bool bTransportConnecting{false};
    bool bTransportConnected{false};
    bool bLoggedConnectFailure{false};
};

UCLASS(ClassGroup=(RampLab), meta=(BlueprintSpawnableComponent))
class RAMPLABINTEGRATION_API URampLabCameraSensorComponent final : public USceneCaptureComponent2D
{
    GENERATED_BODY()
public:
    URampLabCameraSensorComponent();
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    bool CaptureAtSimulationTime(double TimeSeconds);
    [[nodiscard]] uint64 GetMeasuredCaptureCount() const noexcept { return MeasuredCaptureCount; }
    [[nodiscard]] double GetMeanCaptureCostMilliseconds() const noexcept {
        return MeasuredCaptureCount == 0 ? 0.0 : AccumulatedCaptureCostMilliseconds / static_cast<double>(MeasuredCaptureCount);
    }
    UPROPERTY(EditAnywhere, Category="Sensor", meta=(ClampMin="1.0", ClampMax="179.0")) float HorizontalFovDegrees{90.0f};
    UPROPERTY(EditAnywhere, Category="Sensor", meta=(ClampMin="0.1")) float UpdateRateHz{20.0f};
    UPROPERTY(EditAnywhere, Category="Sensor", meta=(ClampMin="1")) int32 ImageWidth{320};
    UPROPERTY(EditAnywhere, Category="Sensor", meta=(ClampMin="1")) int32 ImageHeight{180};
    UPROPERTY(EditAnywhere, Category="Sensor", meta=(ClampMin="1", ClampMax="65535")) int32 TransportPort{39010};
    UPROPERTY(VisibleAnywhere, Category="Sensor") FRampLabSensorFrameMetadata Metadata;
private:
    TObjectPtr<class UTextureRenderTarget2D> ImageTarget;
    class FSocket* TransportSocket{};
    double NextCaptureSeconds{0.0};
    double NextConnectAttemptSeconds{0.0};
    double AccumulatedCaptureCostMilliseconds{0.0};
    uint64 MeasuredCaptureCount{0};
    bool bTransportConnecting{false};
    bool bTransportConnected{false};
    bool bLoggedConnectFailure{false};
};
