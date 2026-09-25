# Unreal Development on Windows

These steps were validated on Windows 11 with Unreal Engine 5.8, Visual Studio Community 2026 18.10.2, MSVC 19.51.36260, Windows SDK 10.0.26100.0, and CMake 4.4.3.

Run commands from the repository root in PowerShell.

## Build the linked core libraries

```powershell
powershell -ExecutionPolicy Bypass -File .\unreal\RampLabViewer\Scripts\BuildRampLabCore.ps1
```

The script configures `build-unreal-core` with Visual Studio 18 2026, x64, Release, and tests disabled. `RampLabIntegration.Build.cs` links `airside_sim.lib`, `airside_scenario.lib`, and `yaml-cpp.lib` from that tree.

## Build the Unreal editor target

```powershell
& 'C:\Program Files\Epic Games\UE_5.8\Engine\Build\BatchFiles\Build.bat' `
  RampLabViewerEditor Win64 Development `
  "-Project=$PWD\unreal\RampLabViewer\RampLabViewer.uproject" `
  -WaitMutex -NoHotReload
```

The validated build result is `Succeeded`. UBT currently warns that installed MSVC 14.51 is newer than its preferred 14.50 toolchain; this is a compatibility warning, not a build failure.

## Open or launch the demo

Double-click `unreal/RampLabViewer/RampLabViewer.uproject`, or run:

```powershell
& 'C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe' `
  "$PWD\unreal\RampLabViewer\RampLabViewer.uproject"
```

The configured default map is an empty engine world driven by `ARampLabDemoGameMode`; no authored Blueprint level is required. Press Play in the editor. For the directly validated standalone game window:

```powershell
& 'C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe' `
  "$PWD\unreal\RampLabViewer\RampLabViewer.uproject" `
  -game -windowed -ResX=1280 -ResY=720 -NoSplash
```

## Runtime verification

Exercise the control methods and inspect `Saved/Logs/RampLabViewer.log` for `RampLab control check: PASSED`:

```powershell
& 'C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe' `
  "$PWD\unreal\RampLabViewer\RampLabViewer.uproject" `
  -game -windowed -ResX=960 -ResY=540 -NoSplash -RampLabControlCheck
```

Capture the deterministic visual milestones at accelerated developer speed:

```powershell
& 'C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe' `
  "$PWD\unreal\RampLabViewer\RampLabViewer.uproject" `
  -game -windowed -ResX=1280 -ResY=720 -NoSplash `
  -RampLabCapture -RampLabCaptureMultiplier=10
```

The capture multiplier is a debug-only wall-clock accelerator applied on top of the visible operator playback setting. It is accepted only with `-RampLabCapture`; the UI continues to show one of the supported operator speeds and labels the QA acceleration explicitly.

The captures are written to `unreal/RampLabViewer/Saved/Screenshots/RampLab`: start, road closed, reroute/service activity, and near-complete run. `Saved`, `Intermediate`, `Binaries`, `DerivedDataCache`, solution files, and all CMake build trees remain ignored by Git.
