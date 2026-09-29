#include "RampLabSensors.h"

#include "Engine/TextureRenderTarget2D.h"
#include "TextureResource.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

#include <cstdint>

URampLabLidarSensorComponent::URampLabLidarSensorComponent()
{
    PrimaryComponentTick.bCanEverTick = false;
    Metadata.SensorId = TEXT("lidar");
    Metadata.FrameId = TEXT("lidar");
}

bool URampLabLidarSensorComponent::CaptureAtSimulationTime(double TimeSeconds)
{
    if (!FMath::IsFinite(TimeSeconds) || TimeSeconds + 1e-9 < NextCaptureSeconds ||
        RayCount < 2 || MaximumRangeMeters <= MinimumRangeMeters || GetWorld() == nullptr) return false;
    Metadata.TimestampSeconds = TimeSeconds;
    ++Metadata.Sequence;
    Metadata.bValid = true;
    RangesMeters.SetNumUninitialized(RayCount);
    const FTransform SensorPose = GetComponentTransform();
    FCollisionQueryParams Params(SCENE_QUERY_STAT(RampLabLidar), true);
    Params.AddIgnoredActor(GetOwner());
    const float HalfFov = HorizontalFovDegrees * 0.5f;
    for (int32 Beam = 0; Beam < RayCount; ++Beam) {
        const float Fraction = static_cast<float>(Beam) / static_cast<float>(RayCount - 1);
        const float YawOffset = FMath::Lerp(-HalfFov, HalfFov, Fraction);
        const FVector Direction = SensorPose.GetRotation().RotateVector(
            FRotator(0.0f, YawOffset, 0.0f).Vector());
        const FVector Start = SensorPose.GetLocation();
        const FVector End = Start + Direction * MaximumRangeMeters * 100.0f;
        FHitResult Hit;
        const bool bHit = GetWorld()->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, Params);
        const float Range = bHit ? FVector::Distance(Start, Hit.ImpactPoint) / 100.0f : MaximumRangeMeters;
        RangesMeters[Beam] = FMath::Clamp(Range, MinimumRangeMeters, MaximumRangeMeters);
    }
    NextCaptureSeconds = TimeSeconds + 0.1;
    return true;
}

URampLabCameraSensorComponent::URampLabCameraSensorComponent()
{
    PrimaryComponentTick.bCanEverTick = false;
    CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
    bCaptureEveryFrame = false;
    bCaptureOnMovement = false;
    Metadata.SensorId = TEXT("camera_rgb");
    Metadata.FrameId = TEXT("camera");
}

bool URampLabCameraSensorComponent::CaptureAtSimulationTime(double TimeSeconds)
{
    if (!FMath::IsFinite(TimeSeconds) || TimeSeconds + 1e-9 < NextCaptureSeconds || GetWorld() == nullptr) return false;
    if (ImageTarget == nullptr) {
        ImageTarget = NewObject<UTextureRenderTarget2D>(this, TEXT("RampLabCameraTarget"));
        ImageTarget->RenderTargetFormat = ETextureRenderTargetFormat::RTF_RGBA8;
        ImageTarget->InitAutoFormat(ImageWidth, ImageHeight);
        TextureTarget = ImageTarget;
    }
    FOVAngle = HorizontalFovDegrees;
    CaptureScene();
    Metadata.TimestampSeconds = TimeSeconds;
    ++Metadata.Sequence;
    Metadata.bValid = TextureTarget != nullptr;
    if (Metadata.Sequence == 1 && TextureTarget != nullptr) {
        TArray<FColor> Pixels;
        if (FTextureRenderTargetResource* Resource = TextureTarget->GameThread_GetRenderTargetResource()) {
            Resource->ReadPixels(Pixels);
        }
        std::uint64_t PixelDigest = 14695981039346656037ULL;
        uint64 NonBlackPixels = 0;
        for (const FColor Pixel : Pixels) {
            NonBlackPixels += (Pixel.R != 0 || Pixel.G != 0 || Pixel.B != 0) ? 1U : 0U;
            for (const uint8 Channel : {Pixel.R, Pixel.G, Pixel.B}) {
                PixelDigest ^= Channel;
                PixelDigest *= 1099511628211ULL;
            }
        }
        Metadata.bValid = Pixels.Num() == ImageWidth * ImageHeight && NonBlackPixels > 0;
        UE_LOG(LogTemp, Display, TEXT("RampLab camera readback: pixels=%d non_black=%llu digest=%llu valid=%s"),
            Pixels.Num(), static_cast<unsigned long long>(NonBlackPixels),
            static_cast<unsigned long long>(PixelDigest), Metadata.bValid ? TEXT("true") : TEXT("false"));
    }
    NextCaptureSeconds = TimeSeconds + 0.05;
    return Metadata.bValid;
}
