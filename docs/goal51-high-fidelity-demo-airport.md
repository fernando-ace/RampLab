# Goal 51: High-Fidelity RampLab Demo Airport

## Ownership and data flow

The C++23 `airside_sim` engine remains authoritative for simulation time, aircraft and vehicle positions, assignments, service states, routes, permissions, gate occupancy, arrivals, departures, and results. Unreal reads the `SimulationSnapshot` and its `AirportDesign`; it does not choose routes, dispatch equipment, or change outcomes.

The synthetic airport uses its authored local-meter frame: +X east, +Y north, +Z up. The Unreal presentation maps this to `(x * 100 cm, -y * 100 cm, z)` without Cesium or a geographic placement. Runways, surface regions, stands, support pads, graph centerlines, and hold markings derive from simulator design and graph records. Grass extent, runway shoulders, building massing, glazing, lighting, vehicle/aircraft meshes, and equipment details are deterministic presentation geometry. They do not assert surveyed real-world features.

## Presentation

- Runway, taxiway, apron, and service-road surfaces follow the Goal 50 polygons. The runway adds a six-meter shoulder on each side, edge and centerline markings, threshold bars, and 09/27 designations.
- Taxi centerlines, service-road dashes, hold-short bars, and stand outlines/lead-ins derive from the operational design or graph. Graph route segments are hidden in presentation mode; `-RampLabShowRoutes` reveals them for inspection.
- Stands and service areas are labeled. The terminal, GSE depot, and maintenance/hangar massing are placed within their authored support footprints. Fuel staging remains an open paved area. Fixed daylight and a broad turf field give the airport context without external data.
- Aircraft use a 34-meter fuselage, 35-meter main wing, tailplane, vertical tail, two engine nacelles, and landing gear. These generic assemblies fit within the 36-meter wingspan design limit. The scenario's actual fueling truck and baggage tractor/cart have distinct colors, cabs, glass, wheels, tanks or trailer geometry.
- Vehicle positions and headings interpolate on the presentation side. Taxiing aircraft ease toward the latest simulator-authored position and heading. Aircraft and vehicle selection details remain in the operator panel; follow cameras track the selected ID when it is present in the active snapshot.
- While a vehicle is `Servicing`, a deterministic visual attachment places it near its assigned aircraft. Fuel equipment uses a local `(forward +1 m, side +9 m)` offset; baggage equipment uses `(forward -8 m, side -7 m)`. These are presentation offsets only. The engine's node, assignment, and state are untouched.
- Hiding the operator panel still initializes and advances the viewer. `InitializeViewer()` owns simulation reset/readiness; `AttachControlPanel()` only adds the Slate panel and then initializes the viewer.

## Launch and demonstration

From the repository root in PowerShell:

```powershell
& 'C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe' `
  "$PWD\unreal\RampLabViewer\RampLabViewer.uproject" `
  -game -windowed -ResX=1600 -ResY=900 -NoSplash -RampLabDemoAirport
```

The scenario is seed 50 at 10x presentation playback by default. Use **Overview** for the full airport, **Apron** for stands and terminal, **Ramp** for a lower operational view, and **Follow Aircraft** / **Follow Vehicle** for the selected entity. W/A/S/D pans, Q/E rotates, and the mouse wheel zooms. Hide the operator panel with `-RampLabHideControlPanel`; show route and permission geometry with `-RampLabShowRoutes`.

For an active service frame with the fuel truck beside a turnaround aircraft, run at 60x and request the screenshot after 120 seconds. This places the replay near simulation second 7,200, when the fuel truck is servicing the stand-start flights. The camera follows the selected active aircraft:

```powershell
New-Item -ItemType Directory -Force evidence\goal51 | Out-Null
& 'C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe' `
  "$PWD\unreal\RampLabViewer\RampLabViewer.uproject" `
  -game -windowed -ResX=1536 -ResY=864 -NoSplash -RampLabDemoAirport `
  -RampLabHideControlPanel -RampLabPlaybackSpeed=60 -RampLabCameraPreset="Follow Aircraft" `
  -RampLabScreenshot="$PWD\evidence\goal51\hero-turnaround.png" `
  -RampLabScreenshotDelaySeconds=120
```

The capture is also recorded in `evidence/goal51/unreal-runtime.log`: it includes the playback second and each aircraft's state/gate at capture request, plus simulator events and Unreal frame telemetry. Keep the operator panel visible when reviewing IDs and operational states; hide it for the portfolio frame.

## Evidence and limitations

Validation performed on this branch:

- Unreal game target and `RampLabViewerEditor` Development target both built successfully with UE 5.8 and the pinned Cesium for Unreal 2.29.1 plugin. The Editor build was limited to four parallel actions after the host paging-file limit rejected the initial high-parallelism Cesium compile.
- The active seed-50 runtime rendered the synthetic airport from simulator design data without initializing a geographic provider. At 60x, the hidden-panel capture reached simulation second 7,199; the log records active servicing and turnaround events. Observed runtime samples were about 47.8–51.2 FPS at 1536×864, with 7 synchronized aircraft/vehicle entities and no frame deltas over 500 ms in the listed sample.
- Release CTest passed **179/179** tests in `C:\Projects\RampLab\build-final-msvc` at the same base revision. The canonical Goal 50 seed-50 result and two-run byte-identical metric/event hashes are preserved in [the Goal 50 manifest](../evidence/goal50/manifest.json); this Goal 51 work changes only the Unreal presentation and does not modify the C++ simulator or scenario YAML.
- The actual hidden-UI Unreal frame is [hero-turnaround.png](../evidence/goal51/hero-turnaround.png). Unreal runtime log: [unreal-runtime.log](../evidence/goal51/unreal-runtime.log).

The Demo Airport is fictional and does not change KAUO's `DATA_BLOCKED` status. Unreal geometry and attached GSE positions remain presentation approximations. Movement interpolation does not change the discrete-event engine, and Goal 51 does not add aircraft takeoff-roll motion, tug coupling, acceleration, swept-volume collision checks, or physical steering; those remain Goal 52 work.

A fresh full-duration CLI replay was not completed for Goal 51; an attempted run was stopped after prolonged runtime. The in-engine presentation capture reached simulation second 7,199 and is not evidence of the complete 12,747-second replay. Goal 50's immutable canonical seed-50 replay remains documented at the unchanged scenario hash. `evidence/goal51/manifest.json` records hashes and the exact boundary of the Goal 51 evidence.
