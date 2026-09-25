# RampLab Airside Sim

RampLab is a portable C++23 discrete-event simulation engine for airport ramp operations. The engine is authoritative, deterministic, headless, and presentation-independent. Milestone 2 adds stable integration boundaries for a future Unreal Engine adapter without adding Unreal or any rendering dependency.

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

## Architecture

The core `airside_sim` library contains no YAML, UI, network, Unreal, ROS2, or platform rendering code. `airside_scenario` is the only target that knows about yaml-cpp. Both the CLI and future adapters consume ordinary C++ domain values.

```text
YAML Scenario -> airside_scenario -> Scenario -> airside_sim
                                             /              \
                                    Event Stream          Snapshot API
                                             \              /
                                              External consumers
```

See [architecture.md](docs/architecture.md) and [unreal-integration.md](docs/unreal-integration.md).

## Repository layout

```text
apps/airside_cli/          CLI adapter and diagnostic serializers
include/airside/
  agents/                 Aircraft and service vehicles
  core/                   Time, IDs, event queue, structured events
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
```

## Windows prerequisites

- Windows 11
- Visual Studio 2022 with **Desktop development with C++**
- CMake 3.24 or newer
- Git and network access for the first dependency configuration

CMake reproducibly fetches yaml-cpp 0.8.0 and GoogleTest 1.17.0. Open **Developer PowerShell for VS 2022** in the repository root:

```powershell
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Warnings are enabled with `/W4 /permissive- /Zc:__cplusplus` on MSVC and `-Wall -Wextra -Wpedantic -Wconversion -Wshadow` on GCC/Clang.

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
- MSVC should be validated on a machine with Visual Studio 2022 before embedding into Unreal.

## Next milestone

Build the smallest Unreal plugin adapter that statically links or compiles the RampLab core, creates mirror Actors from an initial snapshot, advances the engine independently of frame rate, interpolates active vehicle journeys, and reacts to structured events. Do not move simulation authority into Unreal Actors.
