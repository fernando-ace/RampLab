# Airside Sim

Airside Sim is a portable C++23 discrete-event simulation foundation for airport ramp operations. It is the first milestone toward a digital-twin platform with individual agents, scenario experiments, external visualization, robotics integration, and simulated sensors. This milestone deliberately remains headless: the engine has no presentation, wall-clock, Unreal Engine, ROS2, database, or cloud dependency.

## Milestone 1

The included baseline scenario models three aircraft at three gates, one fuel truck, and one baggage cart. Each aircraft must complete fueling and baggage service before departure. A road network with alternate paths drives vehicle travel time; FIFO fleet allocation creates resource contention; and a scheduled road closure forces later vehicles to reroute.

The implementation includes:

- an explicit simulation clock independent of wall-clock time;
- a chronological event queue with stable insertion-order tie breaking;
- strongly typed aircraft, vehicle, gate, node, edge, and task IDs;
- a mutable airport road graph with coordinates and edge availability;
- deterministic A* routing using an admissible Euclidean heuristic;
- explicit aircraft, service-task, and service-vehicle state machines;
- FIFO fuel and baggage resource pools;
- configurable service durations and generic road-availability events;
- per-aircraft turnaround, delay, and service-waiting metrics;
- per-fleet utilization and aggregate turnaround metrics;
- a seeded CLI with event logging and faster-than-real-time measurement;
- GoogleTest coverage for scheduler, routing, state, allocation, workflow, disruption, metrics, and determinism.

## Architecture

`airside_sim` is a static library. The `airside_cli` executable is a thin presentation layer that constructs a scenario, runs the engine, and formats its immutable result. Simulation components exchange strong IDs and events; they do not hold presentation objects or uncontrolled global state.

```text
Scenario
   |
   v
Simulation Engine --> Event Scheduler
   |                    |
   +--> Agents          +--> deterministic timestamp/sequence order
   +--> Resources
   +--> Routing
   |
   v
Metrics / Result --> CLI (or a future adapter)
```

See [docs/architecture.md](docs/architecture.md) for component responsibilities and future integration boundaries.

## Repository layout

```text
airside-sim/
|-- CMakeLists.txt
|-- apps/airside_cli/       # Console adapter and report formatting
|-- include/airside/
|   |-- core/               # Time, IDs, events, scheduler
|   |-- world/              # Airport graph and baseline scenario
|   |-- routing/            # A* and route value type
|   |-- agents/             # Aircraft and vehicle state models
|   |-- operations/         # Resource pools and event-driven workflow
|   `-- metrics/            # Independently testable calculations
|-- src/                    # Simulation-library implementation
|-- tests/                  # GoogleTest suites by subsystem
`-- docs/architecture.md
```

## Windows prerequisites

- Windows 11
- Visual Studio 2022 with **Desktop development with C++** and a current MSVC toolset
- CMake 3.24 or newer
- Git (CMake fetches GoogleTest v1.17.0 during test configuration)

Open **Developer PowerShell for VS 2022** at the repository root.

## Build and test

```powershell
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The project also builds with a standards-conforming GCC/Clang toolchain. For a single-configuration generator, omit `-C Release` when running CTest.

## Run the CLI

From a Visual Studio multi-configuration build:

```powershell
.\build\Release\airside_cli.exe --scenario baseline --seed 42
```

Use `--quiet` to suppress the event trace while retaining the final report. `--help` lists the supported arguments. No delay or sleep is added in verbose mode; the log is emitted as events are processed.

Representative deterministic domain results for seed 42 are:

```text
00:04:00  AX202 waiting for fuel resource
00:05:00  road edge 4 closed
00:14:00  FuelTruck-1 assigned to AX202 (route 1->3->5, 240 sec)
...
AX101  turnaround 25.0 min, departure delay 0.0 min
AX202  turnaround 26.0 min, departure delay 0.0 min
AX303  turnaround 39.0 min, departure delay 12.0 min
Average turnaround: 30.0 min
Delayed aircraft: 1 / 3
```

Execution time and the resulting real-time multiplier vary by machine and build configuration.

## Determinism

Events sharing a timestamp execute in the order they were scheduled. Resource queues are FIFO, available vehicle IDs and graph adjacency are ordered, and A* has explicit deterministic tie breaking. A `std::mt19937_64` is initialized from `--seed` and reserved for future stochastic inputs; this milestone's baseline has no random distributions. The same scenario and seed therefore produce identical events and metrics.

## Metric definitions

- **Turnaround:** actual departure minus actual arrival.
- **Departure delay:** maximum of zero and actual departure minus scheduled departure.
- **Service waiting:** for each required task, service start minus request time, summed per aircraft. This includes queueing and dispatched vehicle travel.
- **Fleet utilization:** total travel plus active service time for vehicles of a fleet, divided by simulated duration times fleet size.
- **Average turnaround:** arithmetic mean of completed aircraft turnaround times.
- **Delayed aircraft:** count with positive departure delay.

The simulated duration ends after the final queued event, including the last service vehicle's return to depot.

## Key design decisions

- The engine owns simulation time and never reads wall-clock time; the CLI alone measures execution duration.
- Scenarios are data values containing graph, agents, durations, and disruptions.
- Routes retain ordered node and edge IDs for a future renderer to interpolate.
- Resource allocation is separate from vehicle motion and lifecycle state.
- The engine returns domain state, event history, and metrics without depending on a UI API.
- There is no singleton or shared ownership; the simulation owns its scenario state by value.

## Current limitations

- Gate occupancy, pushback tractors, runway/taxiway traffic, collision avoidance, and vehicle congestion are not modeled.
- Vehicle movement is event-to-event rather than continuously integrated.
- A route is fixed for a trip once dispatched; a closure affects newly calculated routes, not a vehicle already in transit.
- Service durations are deterministic and each fleet contains one baseline vehicle.
- The scenario is compiled C++ data; there is no external scenario file format yet.
- The performance number is an illustrative single-run wall-clock measurement, not a benchmark suite.

## Recommended next milestone

Add a versioned, read-only simulation-state snapshot and event-stream adapter suitable for Unreal Engine visualization. Keep Unreal code in a separate adapter target, preserve headless execution, and make the engine the sole authority for simulation time and state. ROS2, experiment runners, sensors, and parallel execution should remain later milestones.
