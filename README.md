# RampLab Airside Sim

RampLab is a portable C++23 discrete-event simulation engine for airport ramp operations. The engine is authoritative, deterministic, headless, and presentation-independent. The optional Unreal Engine viewer consumes the same snapshots and events to visualize the simulation without moving domain decisions into Actors.

## Capabilities

- Explicit simulation time and stable discrete-event ordering.
- Strongly typed entity IDs and guarded aircraft/vehicle state machines.
- Airport graph, deterministic A* routing, closures, and rerouting.
- Concurrent service workflows with FIFO resource contention.
- Versioned, retained-by-value simulation snapshots.
- Structured event records delivered to zero or more read-only sinks.
- Stepwise `finished()` / `advance()` execution for external consumers.
- Validated YAML scenarios through an isolated loader library.
- Human-readable snapshot diagnostics and optional JSON Lines event recording.
- Turnaround, delay, waiting-time, and fleet-utilization metrics.
- Parallel deterministic parameter sweeps with reproducible CSV/JSON experiment results.
- Separate fixed-step ground-vehicle autonomy simulation with deterministic GNSS, IMU, odometry, LiDAR, A* route following, collision checking, and sensor-noise experiments.
- Optional Unreal 5.8/Cesium digital twin anchored at Auburn University Regional Airport, with entity inspection and actual scenario comparison.
- Optional Unreal autonomy mode with the closed-loop tug, A* route, obstacles, GNSS estimate, and LiDAR overlays.
- Optional native Windows ROS 2 bridge and separately running external controller for the tug autonomy simulation.

## Architecture

The core `airside_sim` and `airside_autonomy` libraries contain no YAML, UI, network, Unreal, ROS2, or platform rendering code. YAML parsing stays in the scenario-loader targets. Both the CLI and viewer adapters consume ordinary C++ domain values.

```text
                         RampLab
                           |
             +-------------+--------------+
             |                            |
     Operational Simulation       Autonomy Simulation
     discrete events / services    fixed-step tug / sensors
             |                            |
             +-------------+--------------+
                           |
                   Experiment Runners
                           |
                    Aggregated results
             (Unreal mirrors both simulation domains)
```

See [architecture.md](docs/architecture.md) and [unreal-integration.md](docs/unreal-integration.md).
The optional aircraft task-DAG and scheduler layer is described in [turnaround operations](docs/turnaround-operations.md).

## Repository layout

```text
apps/airside_cli/          CLI adapter and diagnostic serializers
apps/airside_experiment/   Parallel experiment CLI
experiments/               Versioned experiment definitions
include/airside/
  agents/                 Aircraft and service vehicles
  autonomy/               Fixed-step vehicle, observations, controllers, experiments
  core/                   Time, IDs, event queue, structured events
  experiment/             Typed sweeps, worker pool, results, statistics
  integration/            Snapshot schema
  metrics/                Independently testable metrics
  operations/             Resource pools and simulation coordinator
  routing/                Deterministic A*
  scenario/               Format boundary for external scenarios
  world/                  Graph, coordinates, and gates
scenarios/                Human-authored YAML scenarios
src/                      Library implementations
tests/                    GoogleTest suites
docs/                     Architecture and integration contracts
unreal/RampLabViewer/     Optional Unreal Engine 5.8 visualization
```

## Windows prerequisites

- Windows 11
- Visual Studio with **Desktop development with C++** and an MSVC toolset
- CMake 3.24 or newer
- Git and network access for the first dependency configuration

CMake reproducibly fetches yaml-cpp 0.8.0 and GoogleTest 1.17.0. Open a **Developer PowerShell for Visual Studio** in the repository root:

```powershell
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Warnings are enabled with `/W4 /permissive- /Zc:__cplusplus` on MSVC and `-Wall -Wextra -Wpedantic -Wconversion -Wshadow` on GCC/Clang.

## Headless mode

The CMake build, CLI, and tests remain independent of Unreal Engine. This is the primary simulation and experiment workflow.

## Run scenarios

```powershell
.\build\Release\airside_cli.exe --scenario scenarios\baseline.yaml --seed 42
.\build\Release\airside_cli.exe --scenario scenarios\high_capacity.yaml --seed 42
```

If `--seed` is omitted, the file's `default_seed` is used. A CLI seed always takes precedence. The default scenario path is `scenarios/baseline.yaml` relative to the current directory.

Other options:

```powershell
# Final report without the event trace
.\build\Release\airside_cli.exe --scenario scenarios\baseline.yaml --quiet

# Inspect a concise snapshot after every processed scheduled event
.\build\Release\airside_cli.exe --scenario scenarios\baseline.yaml --quiet --dump-snapshots

# Record structured events outside the simulation core
.\build\Release\airside_cli.exe --scenario scenarios\baseline.yaml --quiet --record-events events.jsonl
```

## Visualization mode

The optional viewer requires Unreal Engine 5.8 and a compatible Windows MSVC toolchain. Build the core library used by Unreal, build the editor target, then launch the project:

```powershell
powershell -ExecutionPolicy Bypass -File .\unreal\RampLabViewer\Scripts\BuildRampLabCore.ps1
powershell -ExecutionPolicy Bypass -File .\unreal\RampLabViewer\Scripts\InstallCesium.ps1
& 'C:\Program Files\Epic Games\UE_5.8\Engine\Build\BatchFiles\Build.bat' RampLabViewerEditor Win64 Development "-Project=$PWD\unreal\RampLabViewer\RampLabViewer.uproject" -WaitMutex -NoHotReload
& 'C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe' "$PWD\unreal\RampLabViewer\RampLabViewer.uproject"
```

For streamed Auburn geographic context, copy `unreal/RampLabViewer/.env.example` to the ignored `.env.local` and add an ion token authorized for terrain asset 1 and imagery asset 2. The viewer is anchored to KAUO on a Cesium WGS84 globe while the authoritative simulation remains in local meters. See [Unreal integration](docs/unreal-integration.md), [Unreal development](docs/unreal-development.md), and [visual assets/data sources](docs/assets.md).

The viewer starts with `scenarios/baseline.yaml` and its configured seed. Its compact Slate operator panel provides Play/Pause, Reset, 1x/5x/10x/20x playback, baseline/high-capacity selection, entity inspection, four camera presets, engine-derived comparison metrics, and structured events. See [Unreal development](docs/unreal-development.md) for the exact validated workflow, camera controls, demo mode, and diagnostic launch flags.

## Parallel experiments

`airside_experiment` expands external YAML definitions into stable parameter cases and explicit seeds, then executes independent copies of the same `Simulation` engine through a bounded worker pool. The YAML loader is separate from the reusable execution library, and no experiment target depends on Unreal.

```powershell
.\build\Release\airside_experiment.exe --experiment experiments\small_validation.yaml --dry-run
.\build\Release\airside_experiment.exe --experiment experiments\capacity_sweep.yaml --workers 4
```

Each completed experiment writes `runs.csv`, `summary.csv`, and `experiment.json`. The batch path discards per-event history and snapshots while retaining final run metrics. See [experiments.md](docs/experiments.md) for the schema, supported typed overrides, seed rules, statistics, output contracts, worker policy, and development benchmark.

## Ground-vehicle autonomy

```powershell
.\build\Release\airside_autonomy.exe --scenario scenarios\autonomy_tug.yaml --seed 42
.\build\Release\airside_autonomy.exe --scenario scenarios\autonomy_tug.yaml --seed 42 --record-trajectory results\tug.csv
.\build\Release\airside_autonomy.exe --scenario scenarios\autonomy_safety_stop.yaml --seed 42
.\build\Release\airside_autonomy_experiment.exe --experiment experiments\autonomy_noise_validation.yaml --workers 4
.\build\Release\airside_fleet.exe --scenario scenarios\autonomy_fleet.yaml --seed 42 --csv results\fleet.csv --json results\fleet.json
.\build\Release\airside_fleet.exe --scenario scenarios\autonomy_fleet_dynamic_closure.yaml --seed 42
```

The optional autonomy library and headless tools build without Unreal or ROS 2. The autonomy architecture, equations, and sensor assumptions are documented in [autonomy.md](docs/autonomy.md). The native ROS 2 workspace, activation sequence, topics, external mission, and timeout behavior are documented in [ros2.md](docs/ros2.md).

## Snapshot API

`Simulation::snapshot()` returns a self-contained `SimulationSnapshot`; no mutable engine references escape. Schema version 1 includes:

- simulation time;
- aircraft identity, state, gate, logical node, schedule, actual times, and service-task status;
- vehicle identity, state, current/destination nodes, assignment, and active journey;
- each active journey's route and timed segments for multi-edge interpolation;
- gate positions, occupancy, and availability;
- road-node positions and road-edge availability.

```cpp
Simulation simulation{scenario, seed};
while (!simulation.finished()) {
    simulation.advance();
    SimulationSnapshot retained_copy = simulation.snapshot();
    renderer.consume(retained_copy);
}
```

`kSnapshotSchemaVersion` changes only when the public snapshot shape or interpretation becomes incompatible. Additive fields should be documented; breaking consumers requires a version increment.

## Spatial convention

`Vec2` uses airport-local meters. `x_m` increases east and `y_m` increases north, forming a right-handed 2D ground plane. Adapters choose their own origin, scale, handedness, and vertical axis. An Unreal adapter will normally convert meters to centimeters and map the 2D axes without exposing Unreal types to the engine.

## Structured events

`ISimulationEventSink::on_event(const SimulationEventRecord&)` receives immutable facts in deterministic sequence order. Records contain typed aircraft, vehicle, gate, edge, service, state-transition, and route fields as applicable. Registering no sink is normal; multiple sinks are supported. CLI text and JSONL are adapter formatting, not engine behavior.

## Scenario format

Only the loader layer depends on YAML. A concise excerpt:

```yaml
name: baseline
default_seed: 42
airport:
  nodes:
    - { id: depot, x_m: 0, y_m: 0 }
    - { id: gate_a1, x_m: 220, y_m: 80 }
  edges:
    - id: depot_gate
      from: depot
      to: gate_a1
      distance_m: 262
      traversal_time_seconds: 180
gates:
  - { id: A1, node: gate_a1 }
fleet:
  vehicles:
    - { id: fuel_1, name: FuelTruck-1, type: fueling, depot_node: depot, speed_mps: 10 }
aircraft:
  - id: AX101
    gate: A1
    scheduled_arrival_seconds: 0
    scheduled_departure_seconds: 1500
    required_services: [fueling, baggage]
```

Validation rejects missing or duplicate IDs, broken node/gate/edge references, invalid or unreachable routes, nonpositive distances/durations/speeds, invalid timestamps, unknown services, empty required collections, and malformed YAML with contextual errors.

## Baseline and comparison scenario

The external baseline preserves Milestone 1 behavior:

```text
Average turnaround: 30.0 min
Delayed aircraft: 1 / 3
Fuel utilization: 88.0%
Baggage utilization: 100.0%
```

`high_capacity.yaml` uses two vehicles per fleet. With seed 42 it produces a 26.0-minute average turnaround and no delayed aircraft, demonstrating that files—not filenames or compiled branches—drive behavior.

## Determinism

For a fixed validated scenario and seed, event history, final snapshot, and metrics are identical. Snapshot calls are const and do not advance the engine. Event sinks receive const records and do not participate in scheduling. Tiny-scenario execution-time multipliers printed by the CLI are illustrative only and are not formal benchmarks.

## Current limitations

- Vehicle motion remains event-to-event; snapshots provide interpolation timing rather than continuous dynamics.
- A road closure affects routes calculated after the closure, not vehicles already in transit.
- Gate occupancy is tracked, but gate allocation is fixed by the scenario.
- Edge traversal time is authoritative; vehicle speed is validated metadata for future movement models.
- There is no scenario schema migration system or binary ABI guarantee yet.
- Event JSONL is a CLI diagnostic format, not a core serialization contract or replay engine.
- The viewer uses lightweight procedural aircraft/vehicle forms and a focused synthetic operational layer rather than production airport assets or surveyed stand geometry.
- Cesium terrain and aerial imagery require network access and an authorized local ion token; the ignored cache is runtime-only.
- The flat operational overlay is visually tuned to the KAUO demo area but does not conform each mesh vertex to terrain elevation.
- Playback and mirroring run on the game thread; a copied-snapshot worker handoff is a later scaling concern.

## Next milestone

Evaluate a ROS2 bridge, simulated sensors, and closed-loop ground-vehicle autonomy against the measured experiment-runner scaling results. Keep the deterministic engine authoritative, use the experiment subsystem for repeatable validation, and add more local performance work first only if profiling identifies an actual scaling constraint.
