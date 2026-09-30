#include "RampLabSensors.h"

#include "Engine/TextureRenderTarget2D.h"
#include "TextureResource.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "IPAddress.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "HAL/PlatformTime.h"

#include <cstdint>
#include <cstring>

namespace {
void append_u16(TArray<uint8>& Bytes, uint16 Value) {
    Bytes.Add(static_cast<uint8>(Value)); Bytes.Add(static_cast<uint8>(Value >> 8U));
}
void append_u32(TArray<uint8>& Bytes, uint32 Value) {
    for (uint32 Shift = 0; Shift < 32; Shift += 8) Bytes.Add(static_cast<uint8>(Value >> Shift));
}
void append_u64(TArray<uint8>& Bytes, uint64 Value) {
    for (uint32 Shift = 0; Shift < 64; Shift += 8) Bytes.Add(static_cast<uint8>(Value >> Shift));
}
void append_float(TArray<uint8>& Bytes, float Value) {
    uint32 Bits{};
    FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
    append_u32(Bytes, Bits);
}
}

URampLabLidarSensorComponent::URampLabLidarSensorComponent()
{
    PrimaryComponentTick.bCanEverTick = false;
    Metadata.SensorId = TEXT("lidar");
    Metadata.FrameId = TEXT("lidar");
}

void URampLabLidarSensorComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    if (TransportSocket != nullptr) {
        TransportSocket->Close();
        ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(TransportSocket);
        TransportSocket = nullptr;
    }
    Super::EndPlay(EndPlayReason);
}

bool URampLabLidarSensorComponent::CaptureAtSimulationTime(double TimeSeconds)
{
    if (!FMath::IsFinite(TimeSeconds) || TimeSeconds + 1e-9 < NextCaptureSeconds ||
        RayCount < 2 || MaximumRangeMeters <= MinimumRangeMeters || GetWorld() == nullptr) return false;
    const double CaptureStartSeconds = FPlatformTime::Seconds();
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
        if (bHit && Range < MaximumRangeMeters) ++TotalHitReturns;
    }
    if (TimeSeconds >= NextConnectAttemptSeconds && TransportSocket == nullptr) {
        NextConnectAttemptSeconds = TimeSeconds + 0.5;
        ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
        TransportSocket = Sockets->CreateSocket(NAME_Stream, TEXT("RampLabLidarTcp"), false);
        bool bValidAddress = false;
        const TSharedRef<FInternetAddr> Address = Sockets->CreateInternetAddr();
        Address->SetIp(TEXT("127.0.0.1"), bValidAddress);
        Address->SetPort(TransportPort);
        if (!bValidAddress || !TransportSocket->SetNonBlocking(true) || !TransportSocket->Connect(*Address)) {
            TransportSocket->Close();
            Sockets->DestroySocket(TransportSocket);
            TransportSocket = nullptr;
            if (!bLoggedConnectFailure) {
                UE_LOG(LogTemp, Warning, TEXT("RampLab LiDAR TCP transport could not connect to 127.0.0.1:%d"), TransportPort);
                bLoggedConnectFailure = true;
            }
        } else {
            bTransportConnecting = true;
        }
    }
    if (TransportSocket != nullptr && bTransportConnecting &&
        TransportSocket->Wait(ESocketWaitConditions::WaitForWrite, FTimespan::Zero())) {
        const ESocketConnectionState State = TransportSocket->GetConnectionState();
        if (State == SCS_Connected) {
            bTransportConnecting = false;
            bTransportConnected = true;
            UE_LOG(LogTemp, Display, TEXT("RampLab LiDAR TCP transport connected to 127.0.0.1:%d"), TransportPort);
        } else if (State == SCS_ConnectionError) {
            ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
            TransportSocket->Close();
            Sockets->DestroySocket(TransportSocket);
            TransportSocket = nullptr;
            bTransportConnecting = false;
            bTransportConnected = false;
            NextConnectAttemptSeconds = TimeSeconds + 0.5;
            if (!bLoggedConnectFailure) {
                UE_LOG(LogTemp, Warning, TEXT("RampLab LiDAR TCP transport could not connect to 127.0.0.1:%d"), TransportPort);
                bLoggedConnectFailure = true;
            }
        }
    }
    if (TransportSocket != nullptr && bTransportConnected) {
        TArray<uint8> Packet;
        Packet.Reserve(48 + RangesMeters.Num() * sizeof(float));
        Packet.Append({static_cast<uint8>('R'), static_cast<uint8>('L'), static_cast<uint8>('L'), static_cast<uint8>('D')});
        append_u16(Packet, 1); // protocol version
        append_u16(Packet, 1); // float32 ranges
        append_u64(Packet, static_cast<uint64>(FMath::RoundToInt64(TimeSeconds * 1.0e9)));
        append_u64(Packet, Metadata.Sequence);
        append_u32(Packet, static_cast<uint32>(RayCount));
        append_float(Packet, FMath::DegreesToRadians(-HorizontalFovDegrees * 0.5f));
        append_float(Packet, FMath::DegreesToRadians(HorizontalFovDegrees / static_cast<float>(RayCount - 1)));
        append_float(Packet, MinimumRangeMeters);
        append_float(Packet, MaximumRangeMeters);
        append_u32(Packet, static_cast<uint32>(RangesMeters.Num()) * sizeof(float));
        for (const float Range : RangesMeters) append_float(Packet, Range);
        int32 Sent = 0;
        int32 Attempts = 0;
        while (Sent < Packet.Num() && Attempts < 20) {
            int32 Written = 0;
            if (!TransportSocket->Send(Packet.GetData() + Sent, Packet.Num() - Sent, Written)) break;
            Sent += Written;
            if (Written == 0) {
                ++Attempts;
                TransportSocket->Wait(ESocketWaitConditions::WaitForWrite, FTimespan::FromMilliseconds(2));
            }
        }
        if (Sent != Packet.Num()) {
            UE_LOG(LogTemp, Warning, TEXT("RampLab LiDAR TCP send stopped after %d of %d bytes; reconnecting"), Sent, Packet.Num());
            TransportSocket->Close();
            ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(TransportSocket);
            TransportSocket = nullptr;
            bTransportConnecting = false;
            bTransportConnected = false;
            NextConnectAttemptSeconds = TimeSeconds + 0.5;
        }
    }
    NextCaptureSeconds = TimeSeconds + 1.0 / FMath::Max(0.1f, UpdateRateHz);
    Metadata.CaptureCostMilliseconds = (FPlatformTime::Seconds() - CaptureStartSeconds) * 1000.0;
    AccumulatedCaptureCostMilliseconds += Metadata.CaptureCostMilliseconds;
    ++MeasuredCaptureCount;
    if (Metadata.Sequence % 100 == 0) {
        UE_LOG(LogTemp, Display, TEXT("RampLab LiDAR cost: seq=%llu rays=%d update_ms=%.4f mean_ms=%.4f mean_hits=%.2f"),
            static_cast<unsigned long long>(Metadata.Sequence), RayCount, Metadata.CaptureCostMilliseconds,
            AccumulatedCaptureCostMilliseconds / static_cast<double>(MeasuredCaptureCount),
            static_cast<double>(TotalHitReturns) / static_cast<double>(MeasuredCaptureCount));
    }
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

void URampLabCameraSensorComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    if (TransportSocket != nullptr) {
        TransportSocket->Close();
        ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(TransportSocket);
        TransportSocket = nullptr;
    }
    Super::EndPlay(EndPlayReason);
}

bool URampLabCameraSensorComponent::CaptureAtSimulationTime(double TimeSeconds)
{
    if (!FMath::IsFinite(TimeSeconds) || TimeSeconds + 1e-9 < NextCaptureSeconds || GetWorld() == nullptr) return false;
    const double CaptureStartSeconds = FPlatformTime::Seconds();
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
    if (TimeSeconds >= NextConnectAttemptSeconds && TransportSocket == nullptr) {
        NextConnectAttemptSeconds = TimeSeconds + 0.5;
        ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
        TransportSocket = Sockets->CreateSocket(NAME_Stream, TEXT("RampLabCameraTcp"), false);
        bool bValidAddress = false;
        const TSharedRef<FInternetAddr> Address = Sockets->CreateInternetAddr();
        Address->SetIp(TEXT("127.0.0.1"), bValidAddress);
        Address->SetPort(TransportPort);
        if (!bValidAddress || !TransportSocket->SetNonBlocking(true) || !TransportSocket->Connect(*Address)) {
            TransportSocket->Close();
            Sockets->DestroySocket(TransportSocket);
            TransportSocket = nullptr;
            if (!bLoggedConnectFailure) {
                UE_LOG(LogTemp, Warning, TEXT("RampLab camera TCP transport could not connect to 127.0.0.1:%d"), TransportPort);
                bLoggedConnectFailure = true;
            }
        } else {
            bTransportConnecting = true;
        }
    }
    if (TransportSocket != nullptr && bTransportConnecting &&
        TransportSocket->Wait(ESocketWaitConditions::WaitForWrite, FTimespan::Zero())) {
        const ESocketConnectionState State = TransportSocket->GetConnectionState();
        if (State == SCS_Connected) {
            bTransportConnecting = false;
            bTransportConnected = true;
            UE_LOG(LogTemp, Display, TEXT("RampLab camera TCP transport connected to 127.0.0.1:%d"), TransportPort);
        } else if (State == SCS_ConnectionError) {
            ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
            TransportSocket->Close();
            Sockets->DestroySocket(TransportSocket);
            TransportSocket = nullptr;
            bTransportConnecting = false;
            bTransportConnected = false;
            NextConnectAttemptSeconds = TimeSeconds + 0.5;
            if (!bLoggedConnectFailure) {
                UE_LOG(LogTemp, Warning, TEXT("RampLab camera TCP transport could not connect to 127.0.0.1:%d"), TransportPort);
                bLoggedConnectFailure = true;
            }
        }
    }
    if ((Metadata.Sequence == 1 || bTransportConnected) && TextureTarget != nullptr) {
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
        if (Metadata.Sequence == 1) {
            UE_LOG(LogTemp, Display, TEXT("RampLab camera readback: pixels=%d non_black=%llu digest=%llu valid=%s"),
                Pixels.Num(), static_cast<unsigned long long>(NonBlackPixels),
                static_cast<unsigned long long>(PixelDigest), Metadata.bValid ? TEXT("true") : TEXT("false"));
        }
        if (Metadata.bValid && TransportSocket != nullptr && bTransportConnected) {
            TArray<uint8> Packet;
            Packet.Reserve(40 + Pixels.Num() * sizeof(FColor));
            Packet.Append({static_cast<uint8>('R'), static_cast<uint8>('L'), static_cast<uint8>('S'), static_cast<uint8>('N')});
            append_u16(Packet, 1); // protocol version
            append_u16(Packet, 1); // BGRA8 pixel format
            append_u64(Packet, static_cast<uint64>(FMath::RoundToInt64(TimeSeconds * 1.0e9)));
            append_u64(Packet, Metadata.Sequence);
            append_u32(Packet, static_cast<uint32>(ImageWidth));
            append_u32(Packet, static_cast<uint32>(ImageHeight));
            append_u32(Packet, static_cast<uint32>(Pixels.Num() * sizeof(FColor)));
            uint32 FovBits{};
            FMemory::Memcpy(&FovBits, &HorizontalFovDegrees, sizeof(FovBits));
            append_u32(Packet, FovBits);
            Packet.Append(reinterpret_cast<const uint8*>(Pixels.GetData()), Pixels.Num() * sizeof(FColor));
            int32 Sent = 0;
            int32 Attempts = 0;
            while (Sent < Packet.Num() && Attempts < 50) {
                int32 Written = 0;
                if (!TransportSocket->Send(Packet.GetData() + Sent, Packet.Num() - Sent, Written)) break;
                Sent += Written;
                if (Written == 0) {
                    ++Attempts;
                    TransportSocket->Wait(ESocketWaitConditions::WaitForWrite, FTimespan::FromMilliseconds(2));
                }
            }
            if (Sent != Packet.Num()) {
                UE_LOG(LogTemp, Warning, TEXT("RampLab camera TCP send stopped after %d of %d bytes; reconnecting"), Sent, Packet.Num());
                TransportSocket->Close();
                ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(TransportSocket);
                TransportSocket = nullptr;
                bTransportConnecting = false;
                bTransportConnected = false;
                NextConnectAttemptSeconds = TimeSeconds + 0.5;
            }
        }
    }
    NextCaptureSeconds = TimeSeconds + 1.0 / FMath::Max(0.1f, UpdateRateHz);
    Metadata.CaptureCostMilliseconds = (FPlatformTime::Seconds() - CaptureStartSeconds) * 1000.0;
    AccumulatedCaptureCostMilliseconds += Metadata.CaptureCostMilliseconds;
    ++MeasuredCaptureCount;
    if (Metadata.Sequence % 100 == 0) {
        UE_LOG(LogTemp, Display, TEXT("RampLab RGB camera cost: seq=%llu size=%dx%d update_ms=%.4f mean_ms=%.4f (capture/readback/transport)"),
            static_cast<unsigned long long>(Metadata.Sequence), ImageWidth, ImageHeight, Metadata.CaptureCostMilliseconds,
            AccumulatedCaptureCostMilliseconds / static_cast<double>(MeasuredCaptureCount));
    }
    return Metadata.bValid;
}
