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

The module uses C++23 and enables exceptions because scenario loading reports standard C++ exceptions. Exceptions are caught inside the subsystem and converted to an Unreal error log/status; no exception is allowed to escape into Slate or Actor code. Unreal types never enter the core headers.

## Authority and lifecycle

RampLab owns simulation time, arrivals, resource queues, assignments, routes, road state, services, departures, and metrics. Unreal owns only playback intent and presentation. The subsystem loads the scenario, constructs `Simulation`, registers itself as a structured event sink, takes snapshots, and advances whole engine events only when `next_event_time()` is at or before the visual playback clock.

On each authoritative change it takes a new value snapshot. Road, gate, aircraft, and vehicle mirrors are created once and keyed by their strong-ID numeric value; Tick reconciles visibility, material, state labels, and positions without recreating topology or running pathfinding.

Reset reconstructs the baseline from the same YAML file and seed 42. That clears the event feed and playback clock, then performs the same time-zero initialization, so reset is deterministic.

## Scenario lookup

Development builds resolve `../../scenarios/baseline.yaml` from `FPaths::ProjectDir()`, which reaches the repository's single authoritative scenario. A second candidate named `Scenarios/baseline.yaml` beside the executable is reserved for a future packaged build. A missing or invalid scenario produces a visible status and `LogRampLab` error rather than a silent fallback or Blueprint copy.

## Coordinates and movement

Standalone `visualization::CoordinateTransform` is the single conversion abstraction. The viewer uses airport-local `+x` east and `+y` north directly as Unreal `X/Y`, converts meters to centimeters with scale 100, and supplies visual `Z` heights separately. Its transform also supports an origin offset and optional Y flip; tests cover scale, offset, and handedness.

`visualization::sample_journey` selects the active timed segment and linearly interpolates its endpoint node coordinates. It handles complete multi-edge journeys, clamps before departure and at the exact destination, and rejects missing/malformed data. This position is visual only. Arrival and route selection remain scheduled engine decisions.

## Events and snapshots

The initial snapshot creates the road graph, three gates, three aircraft mirrors, and two service-vehicle mirrors. Later snapshots reconcile every current state. Structured records feed the on-screen recent-event list and Unreal log, including assignment, departure, arrival, service, road closure, readiness, and aircraft departure records. The viewer never parses CLI output or JSONL.

Road availability changes its segment material. Aircraft are hidden while scheduled and after departure, visible at their assigned gate while active, and change material when ready. Fuel and baggage vehicles use different sizes/colors and their labels show Idle, Assigned, Traveling, Servicing, or Returning.

## Playback

The default is auto-play at 10x. The Slate panel calls subsystem methods for Play/Pause, Reset, and 1x/5x/10x/20x. Each render frame adds `DeltaTime * speed` to a visual clock, then processes all scheduled events whose timestamps do not exceed that target. Journey interpolation samples the same clock. Rendering cadence therefore changes visual sampling frequency, not simulation ordering or results.

For runtime verification, `-RampLabControlCheck` exercises the exact methods bound to the controls, verifies pause freezes the clock, resume advances it, reset returns to time zero/seed 42, and logs PASS/FAIL. `-RampLabCapture -RampLabPlaybackSpeed=100` produces four diagnostic screenshots under `Saved/Screenshots/RampLab` and logs the completed metrics.

## Known limitations

- Basic engine cubes, text, and an orthographic camera favor architectural proof over presentation quality.
- Aircraft appear at their assigned gates; taxi and pushback paths are not part of the current domain snapshot.
- Existing in-transit journeys are not recalculated by a later closure, matching core behavior.
- Development scenario lookup is validated; packaged scenario staging/cooking is not yet implemented.
- The subsystem currently advances on the game thread. Large scenarios may later need copied snapshot/event batches from a worker, without making the engine concurrent.
- The UE 5.8 build succeeds with MSVC 14.51 but Unreal Build Tool warns that 14.50 is its preferred compiler family.
