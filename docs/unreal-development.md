# Unreal Development on Windows

These steps were validated on Windows 11 with Unreal Engine 5.8.3, Visual Studio 18, the MSVC 14.44 toolset, Windows SDK 10.0.26100.0, and CMake 4.4.3.

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

For a fresh primary checkout, keep the authorized ion token in an ignored local environment file or an inherited process environment variable. A linked worktree can resolve the primary checkout's existing local environment file without copying it. Never pass the token on the command line or commit it. Use `Scripts/ValidateCesiumAccess.ps1` to verify discovery and permissions safely and `Scripts/LaunchRampLabViewer.ps1` for the reproducible startup path.

The script configures `build-unreal-core-v143` with Visual Studio 18 2026, x64, Release, tests disabled, and toolset v143 14.44 to match Unreal's linker runtime. `RampLabIntegration.Build.cs` links the operations, autonomy, scenario, and yaml-cpp libraries from that tree.

## Build the Unreal editor target

```powershell
& 'C:\Program Files\Epic Games\UE_5.8\Engine\Build\BatchFiles\Build.bat' `
  RampLabViewerEditor Win64 Development `
  "-Project=$PWD\unreal\RampLabViewer\RampLabViewer.uproject" `
  -WaitMutex -NoHotReload
```

The validated build result is `Succeeded` with MSVC 14.44. The linked core libraries and Unreal modules must use compatible MSVC toolsets.

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

## Turnaround operator panel validation

Launch a visible, accelerated turnaround scenario and inspect `Saved/Logs/RampLabGoal14Turnaround.log` for task transitions and the fleet summary:

```powershell
& 'C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe' `
  "$PWD\unreal\RampLabViewer\RampLabViewer.uproject" `
  -game -windowed -ResX=1280 -ResY=720 -NoSplash `
  -RampLabTurnaroundValidation `
  -abslog="$PWD\unreal\RampLabViewer\Saved\Logs\RampLabGoal15Turnaround.log"
```

The validation mode selects `turnaround_normal` at 20x playback. At completion the log reports completed/failed turnarounds, task and fleet reassignments, fleet collisions and minimum separation, reservation/contention counts, outstanding reservations, unresolved requests, and the Recent Events panel contents. The visible run passed with 1/1 completed, zero failures/collisions, 8.0 m minimum separation, and zero unresolved requests; the event panel recorded departure.

For Goal 15, the same `-RampLabTurnaroundValidation` mode selects `turnaround_flight_bank_outage`. The operations panel shows each ground vehicle's availability, assignment, and position alongside aircraft departure delays, task progress, reassignment count, collision metrics, and recent events. The completion log reports outage/reassignment IDs and times, task-completion and departure totals, and fleet safety counters. The validated windowed run reported 3/3 completed, 18 task completions, 3 departures, outage vehicle 2/task 3 at 130 s, reassignment to vehicle 3, 9/9 service requests completed, zero collisions, and 2.111 m minimum separation. The run validates simulation execution and panel-fed state; it does not establish real-airport performance.

Goal 16 surface scenarios are selected with `-RampLabSurfaceTraffic` and `-RampLabSurfaceDisruption`, or with the Surface Traffic / Surface Closure control-panel buttons. Aircraft meshes follow the core snapshot's simulated surface position and heading; the viewer draws the active node route, closed graph segments, and state label. The operations panel reports taxiing, waiting, departure queue, surface wait time, and reroutes. These features remain read-only renderings of the C++ simulation.

The rebuilt UE 5.8 Editor target ran both scenarios in visible accelerated windowed sessions. The control session reported 3/3 departures, 8.845 departures/hour, two traffic waits totaling 95 s, 735 taxi seconds over 672 m, 10 s runway queue, zero safe failures/collisions, and minimum sampled spacing of 30.000 m aircraft-aircraft and 80.083 m aircraft-ground. The closure session reported 3/3 departures, 8.464 departures/hour, one reroute, two waits totaling 64 s, 800 taxi seconds over 768 m, zero failures/collisions, and 38.588 m / 80.083 m minimum spacing. The completion log and operations panel use metrics returned by the C++ simulation. These remain synthetic, sampled planar results.

The capture sequence now includes the Auburn overview, five operational milestones, and three autonomy frames (depot/route, obstacle sensing, Gate A2 result) under `unreal/RampLabViewer/Saved/Screenshots/RampLab`. The Autonomy scenario uses the same fixed-step autonomy library and controller as the headless CLI; the renderer displays its snapshot, GNSS estimate, route, obstacle circles, LiDAR returns, and trajectory. `Saved`, `Intermediate`, `Binaries`, `DerivedDataCache`, solution files, and all CMake build trees remain ignored by Git.

For the deterministic two-scenario presentation flow, launch with:

```powershell
& 'C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe' `
  "$PWD\unreal\RampLabViewer\RampLabViewer.uproject" `
  -game -windowed -ResX=1600 -ResY=900 -log -RampLabDemo
```

The demo runs the real baseline scenario, the real high-capacity YAML, and then the sensor-driven Autonomy mission. `-RampLabCapture -RampLabCaptureMultiplier=10` captures their actual simulated states. The multiplier accelerates wall-clock capture only; the visible operator playback remains one of 1x, 5x, 10x, or 20x and the fixed simulation step/outcomes are unchanged.

Camera controls are `W/A/S/D` to pan, `Q/E` to rotate, and mouse wheel to zoom. The UI also provides Overview, Ramp, Gate A2, and Service Roads presets.

## Goal 18 integrated lifecycle validation

Build the linked C++ core and Editor target using the commands above, then launch the visible seed-42 lifecycle control scenario:

```powershell
& 'C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe' `
  "$PWD\unreal\RampLabViewer\RampLabViewer.uproject" `
  -game -windowed -ResX=1280 -ResY=720 -NoSplash -RampLabGoal18Validation `
  "-abslog=$PWD\unreal\RampLabViewer\Saved\Logs\RampLabGoal18.log"
```

The flag selects `turnaround_lifecycle.yaml` at 20x playback. The operator panel reports lifecycle phase, gate, service progress, gate wait, runway wait, and departure state. On completion, `RampLabGoal18.log` records the ordered arrival-taxi-in, gate, turnaround, pushback/taxi-out, runway-queue, and departure events for each integrated aircraft, plus aggregate completions and sampled collision/separation results. The viewer renders C++ snapshots and events; it does not implement airport scheduling. Use the disrupted YAML with the CLI or ROS observer for the matching delay comparison.

The visible final run completed both integrated lifecycles and all 3/3 departures. The log traced aircraft IDs 2 and 3 from taxi-in through gate, turnaround, taxi-out, runway queue, and departure. It reported 5/5 runway operations, zero aircraft-aircraft or aircraft-ground collisions, 30.000 m minimum sampled aircraft spacing, and 80.000 m aircraft-ground spacing.
