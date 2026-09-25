# Unreal Development on Windows

These steps were validated on Windows 11 with Unreal Engine 5.8, Visual Studio Community 2026 18.10.2, MSVC 19.51.36260, Windows SDK 10.0.26100.0, and CMake 4.4.3.

Run commands from the repository root in PowerShell.

## Build the linked core libraries

```powershell
powershell -ExecutionPolicy Bypass -File .\unreal\RampLabViewer\Scripts\BuildRampLabCore.ps1
```

Install the pinned official Cesium dependency once per checkout:

```powershell
powershell -ExecutionPolicy Bypass -File .\unreal\RampLabViewer\Scripts\InstallCesium.ps1
```

Cesium 2.29.1's official `57` archive supports UE 5.7/5.8 but declares 5.7 in its descriptor. After verifying the archive checksum, the installer normalizes only the ignored project-local descriptor to 5.8 so unattended launches do not reject the plugin at the compatibility prompt. It does not modify the Unreal installation.

Copy `unreal/RampLabViewer/.env.example` to `.env.local`, then replace the placeholder with an ion token authorized for Cesium World Terrain (asset 1) and Bing Maps Aerial (asset 2). `.env.local` is ignored by Git. Do not pass the token on the command line or commit it.

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
  -game -windowed -ResX=1600 -ResY=900 -NoSplash `
  -RampLabCapture -RampLabCaptureMultiplier=10
```

The capture multiplier is a debug-only wall-clock accelerator applied on top of the visible operator playback setting. It is accepted only with `-RampLabCapture`; the UI continues to show one of the supported operator speeds and labels the QA acceleration explicitly.

The six captures are written to `unreal/RampLabViewer/Saved/Screenshots/RampLab`: Auburn overview, baseline operations, road closure, alternate route, baseline result, and high-capacity comparison. `Saved`, `Intermediate`, `Binaries`, `DerivedDataCache`, solution files, and all CMake build trees remain ignored by Git.

For the deterministic two-scenario presentation flow, launch with:

```powershell
& 'C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe' `
  "$PWD\unreal\RampLabViewer\RampLabViewer.uproject" `
  -game -windowed -ResX=1600 -ResY=900 -log -RampLabDemo
```

The demo runs the real baseline scenario, pauses on its final result, switches to the real high-capacity YAML, and leaves the comparison visible. `-RampLabCapture -RampLabCaptureMultiplier=10` adds ignored runtime captures for overview, operations, closure, reroute, baseline results, and high-capacity comparison. The multiplier accelerates wall-clock capture only; the visible operator playback remains one of 1x, 5x, 10x, or 20x and the simulation timeline/outcomes are unchanged.

Camera controls are `W/A/S/D` to pan, `Q/E` to rotate, and mouse wheel to zoom. The UI also provides Overview, Ramp, Gate A2, and Service Roads presets.
