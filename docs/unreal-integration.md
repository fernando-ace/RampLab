# Unreal Engine Integration

## Implemented shape

```text
scenarios/baseline.yaml
          |
          v
airside_scenario + airside_sim static libraries
          |
          +-- SimulationSnapshot
          `-- SimulationEventRecord
                       |
                       v
          URampLabSimulationSubsystem
                       |
                       v
              stable mirror Actors
```

The Unreal 5.8 project is `unreal/RampLabViewer`. Its `RampLabIntegration` module links Release static libraries produced by the normal CMake project. This was selected over compiling duplicate sources in Unreal or adding a DLL/C ABI: the core retains one standalone build, yaml-cpp remains isolated in `airside_scenario`, and no deployment-time ABI boundary is introduced.

## Auburn digital-twin placement

The viewer anchors its Cesium georeference at the FAA-published airport reference point for Auburn University Regional Airport (KAUO): latitude `32.6151667` and longitude `-85.4340000`. The FAA field elevation is `236.83 m`; NOAA NGS GEOID18 reports `-28.561 m` geoid height at the airport reference point, so the configured approximate ellipsoid height is `208.27 m`. `OperationalLayerHeightCm` remains tunable for small mesh/terrain separation and is visually checked against streamed terrain.

The transform is deliberately outside the simulation engine:

```text
RampLab local meters
  x configurable scale
  rotate local +X to its configured true bearing (clockwise from north)
  + configurable airport-local east/north origin offset
  convert meters to Unreal centimeters (east, south, up)
        -> Cesium local Unreal frame
        -> WGS84 at the KAUO georeference origin
```

All values live in `[RampLab.AirportPlacement]` in `unreal/RampLabViewer/Config/DefaultGame.ini`. `FRampLabAirportPlacement` is the single adapter used by operational Actors. The C++ simulation remains meter-based and has no Cesium dependency.

## Real versus synthetic data

Cesium World Terrain and Bing Maps Aerial provide real geographic context. The airport reference point, runway dimensions, and runway orientation come from FAA sources. The three gates/stands, schedules, service fleet, service roads, depot, capacity alternatives, resource contention, and road closure are synthetic RampLab operational data. Building blocks and the taxiway/apron operational overlay are presentation geometry, not a survey or an assertion about KAUO's actual operating layout.

The simulation engine remains authoritative for time, state, routes, closures, resource assignments, and metrics. Unreal interpolates and renders the engine snapshot; it does not compute routes or outcomes.

## Cesium runtime configuration

The project enables the official project-local Cesium plugin. `Scripts/InstallCesium.ps1` downloads version 2.29.1 from CesiumGS and checks the pinned SHA-256 before installation. The installed package is ignored by Git.

At runtime, `ARampLabAirportEnvironment` creates the georeference, Cesium World Terrain (ion asset 1), and Bing Maps Aerial overlay (ion asset 2). It reads `RAMPLAB_CESIUM_ION_TOKEN` from the process environment first, then from ignored `unreal/RampLabViewer/.env.local`. Tokens are never logged. Without a token, the viewer still renders the synthetic operational layer and emits an explicit geospatial-status warning rather than pretending that streamed context exists.

The terrain uses Cesium's default 16-pixel maximum screen-space error and a 256 MiB tile cache. Cesium OSM Buildings are intentionally not loaded: they add global streaming work but little value to the focused general-aviation ramp view, where lightweight synthetic terminal/hangar massing is enough to orient the demonstration. Required Cesium/data-provider credits remain enabled on the terrain and imagery overlay.

The module uses C++23 and enables exceptions because scenario loading reports standard C++ exceptions. Exceptions are caught inside the subsystem and converted to an Unreal error log/status; no exception is allowed to escape into Slate or Actor code. Unreal types never enter the core headers.

## Authority and lifecycle

RampLab owns simulation time, arrivals, resource queues, assignments, routes, road state, services, departures, and metrics. Unreal owns only playback intent and presentation. The subsystem loads the scenario, constructs `Simulation`, registers itself as a structured event sink, takes snapshots, and advances whole engine events only when `next_event_time()` is at or before the visual playback clock.

On each authoritative change it takes a new value snapshot. Road, gate, aircraft, and vehicle mirrors are created once and keyed by their strong-ID numeric value; Tick reconciles visibility, material, state labels, and positions without recreating topology or running pathfinding.

Reset reconstructs the baseline from the same YAML file and seed 42. That clears the event feed and playback clock, then performs the same time-zero initialization, so reset is deterministic.

## Scenario lookup

Development builds resolve `../../scenarios/baseline.yaml` from `FPaths::ProjectDir()`, which reaches the repository's single authoritative scenario. A second candidate named `Scenarios/baseline.yaml` beside the executable is reserved for a future packaged build. A missing or invalid scenario produces a visible status and `LogRampLab` error rather than a silent fallback or Blueprint copy.

## Coordinates and movement

Standalone `visualization::CoordinateTransform` continues to cover the non-geographic adapter math. The digital-twin viewer centralizes its geographic mapping in `FRampLabAirportPlacement`: it applies the configured scale, rotates RampLab local `+x` to the configured clockwise-from-true-north bearing, adds the east/north airport offset, maps east to Unreal `+X` and north to Unreal `-Y`, converts meters to centimeters, and supplies visual `Z` separately. Tests cover the shared scale/offset/handedness and journey sampling; the KAUO placement is additionally logged and visually checked at runtime.

`visualization::sample_journey` selects the active timed segment and linearly interpolates its endpoint node coordinates. It handles complete multi-edge journeys, clamps before departure and at the exact destination, and rejects missing/malformed data. This position is visual only. Arrival and route selection remain scheduled engine decisions.

## Events and snapshots

The initial snapshot creates the road graph, three gates, three aircraft mirrors, and two service-vehicle mirrors. Later snapshots reconcile every current state. Structured records feed the on-screen recent-event list and Unreal log, including assignment, departure, arrival, service, road closure, readiness, and aircraft departure records. The viewer never parses CLI output or JSONL.

Road availability changes its segment material. Aircraft are hidden while scheduled and after departure, visible at their assigned gate while active, and change material when ready. Fuel and baggage vehicles use different sizes/colors and their labels show Idle, Assigned, Traveling, Servicing, or Returning.

## Playback

The default is auto-play at 10x. The Slate panel calls subsystem methods for Play/Pause, Reset, and 1x/5x/10x/20x. Each render frame adds `DeltaTime * speed` to a visual clock, then processes all scheduled events whose timestamps do not exceed that target. Journey interpolation samples the same clock. Rendering cadence therefore changes visual sampling frequency, not simulation ordering or results.

For runtime verification, `-RampLabControlCheck` exercises the exact methods bound to the controls, verifies pause freezes the clock, resume advances it, reset returns to time zero/seed 42, scenario selection and entity/camera inspection work, and logs PASS/FAIL. `-RampLabCapture -RampLabCaptureMultiplier=10` applies an explicitly labeled debug-only acceleration on top of the visible operator speed, produces six diagnostic screenshots under `Saved/Screenshots/RampLab`, and logs the completed metrics. The normal viewer accepts and displays only 1x, 5x, 10x, or 20x.

## Known limitations

- Aircraft and service vehicles are intentionally lightweight procedural forms, not detailed production assets.
- Aircraft appear at their assigned gates; taxi and pushback paths are not part of the current domain snapshot.
- Existing in-transit journeys are not recalculated by a later closure, matching core behavior.
- The focused taxiway, apron, stands, service roads, depot, and building massing are synthetic presentation geometry; only the geographic context and documented FAA runway facts are real-world data.
- Cesium terrain/imagery needs runtime network access and an authorized ion token; the operational layer remains available without it.
- The flat operational overlay is tuned for this focused demo and does not conform every mesh vertex to terrain elevation.
- The subsystem currently advances on the game thread. Large scenarios may later need copied snapshot/event batches from a worker, without making the engine concurrent.
- The UE 5.8 build succeeds with MSVC 14.51 but Unreal Build Tool warns that 14.50 is its preferred compiler family.
