# Architecture

## Authority and boundaries

RampLab is a deterministic, headless discrete-event engine. It owns simulation time, state transitions, routing, resource allocation, gate occupancy, and metrics. External systems observe; they never advance individual entities or write engine state.

```text
                  Scenario Source
                       |
                       v
                Scenario Loader
                  (YAML only here)
                       |
                       v
             Simulation Configuration
                       |
                       v
                Simulation Engine
            /          |          \
           v           v           v
 Structured Events  Snapshot API  Experiment runs
           |           |           |
           v           v           v
       Consumers   Consumers   Aggregation/output
```

- **Simulation engine:** authoritative mutable domain model.
- **Turnaround coordinator:** optional deterministic task-DAG scheduler embedded in `Simulation`; mobile fueling/baggage uses operational vehicle routing and abstract cabin/terminal work uses finite-capacity crews.
- **Surface departure coordinator:** optional deterministic aircraft pushback, taxi-edge/node reservations, closure rerouting, and exclusive FIFO runway resource embedded in `Simulation`; it consumes the airport graph and remains authoritative in C++.
- **Snapshot API:** read-only current-state value, safe to retain.
- **Event stream:** ordered immutable facts about what occurred.
- **Renderer:** non-authoritative consumer whose frame rate cannot affect results.

## Target structure

`airside_sim` is the portable core library. `airside_scenario` depends on `airside_sim` and yaml-cpp; the reverse dependency does not exist. The CLI depends on both and owns text/JSONL serialization.

```text
airside_sim
  core          time, strong IDs, internal event queue, public event records
  world         meter-based graph and gate state
  routing       deterministic A*
  agents        aircraft, tasks, service vehicles
  operations    resource allocation and workflow coordination
  integration   snapshot schema
  metrics       completed-run calculations

airside_scenario
  YAML document -> validation -> Scenario domain value

airside_experiment
  typed definitions and overrides -> stable cases -> bounded worker pool
  final run metrics -> Welford/percentile aggregation -> CSV/JSON writers

airside_experiment_yaml
  YAML document -> validation -> ExperimentDefinition

airside_autonomy
  fixed-step continuous vehicle -> seeded sensors -> controller interface
  reuses airside_sim road graph and deterministic A* route

airside_autonomy_scenario
  autonomy YAML + existing airport map scenario -> validated mission values

airside_autonomy_experiment
  independent autonomy simulations -> final-only metrics -> stable run order

airside_cli
  argument parsing, event sinks, snapshot diagnostics, report formatting

airside_experiment executable
  argument parsing, progress, dry-run, output overrides, comparison table

RampLabViewer (optional Unreal project)
  RampLabIntegration   owns Simulation and read-only mirror synchronization
    Autonomy demo       visualizes an actual sensor-driven autonomy run
    AirportPlacement   config-driven local-meter to airport east/north transform
    AirportEnvironment Cesium WGS84 context plus synthetic operational layer
    WorldActor         stable visual mirrors, heading interpolation, cameras
  RampLabViewer        Unreal application target only
```

## Execution model

Construction validates core duration invariants and schedules scenario events. `advance()` removes exactly one internal scheduled event, moves `SimTime`, mutates authoritative state, schedules consequences, and emits zero or more public event records. `next_event_time()` exposes the next scheduling boundary without mutation so a visual clock never processes a future event early. `finished()` reports an empty scheduler. `run()` is a convenience loop over `advance()`.

Internal scheduling and public events are intentionally different types. Scheduler events are implementation commands such as `ServiceCompleted`; public records are integration facts such as `ServiceStarted`, `VehicleDeparted`, or `RoadClosed`.

Same-timestamp scheduler order uses a monotonic insertion sequence. Public records receive their own monotonic sequence in emission order. Sinks are non-owning and receive const records through a `noexcept` callback.

## Snapshot contract

`Simulation::snapshot() const` builds a value representation from authoritative state. It neither advances time nor caches mutable references. A consumer can retain it while the engine continues.

Schema version 1 contains:

- aircraft schedules, actual times, state, logical location, gate, and task status;
- service-vehicle assignment, state, current/destination nodes, and optional journey;
- route nodes and edges plus timed segments for full-path interpolation;
- gates with position, occupancy, enabled state, and derived availability;
- road nodes with coordinates and road edges with distance, traversal time, and availability.

The snapshot copies only integration-facing values. For the current airport size this is intentionally simple and predictable. Future profiling may introduce immutable shared topology blocks, but consumers must continue to see an independent logical snapshot.

`kSnapshotSchemaVersion` must increment for incompatible field or semantic changes. Additive evolution should remain source-compatible where practical and be documented.

## Spatial contract

`Vec2` is airport-local and measured in meters. `+x` is east and `+y` is north. It is a right-handed 2D ground-plane convention. Graph nodes and gates use these coordinates; road distances are meters.

An adapter owns the world transform. The Unreal adapter scales meters to centimeters, rotates local `+x` to a configured true bearing, applies an airport-local east/north offset, and supplies elevation separately. Cesium then places that local Unreal frame at a WGS84 origin. No Unreal or Cesium coordinate/math type enters the core.

## Vehicle interpolation

Movement remains discrete. During travel, authoritative `current_node` remains the journey origin until arrival. A vehicle snapshot supplies:

- origin and destination;
- route node/edge order;
- whole-journey departure and expected-arrival times;
- each segment's endpoints, departure/arrival times, and distance.

A renderer locates the segment containing snapshot time and interpolates between its endpoint coordinates. This visual calculation never changes engine state.

## Gates

Gates are explicit runtime values. An arriving aircraft occupies its assigned gate; an aircraft departure releases it. Arrival at a disabled or occupied gate is rejected. This is deliberately not a dynamic gate allocator.

## Events versus snapshots

Events are best for:

- starting animations and effects;
- alerts and UI notifications;
- audit/history views;
- lightweight recording and diagnostics.

Snapshots are best for:

- initial synchronization;
- current render state;
- recovery after a consumer reconnects;
- vehicle interpolation;
- correcting drift in a mirror model.

A robust adapter uses both: bootstrap or recover from a snapshot, then respond to ordered events while periodically reconciling against snapshots.

## Scenario loading

The loader has three conceptual stages:

```text
YAML nodes -> private ScenarioDocument -> validation/build -> Scenario
```

Only `src/scenario/scenario_loader.cpp` includes yaml-cpp. Text IDs are checked for uniqueness and resolved to strong runtime IDs. References, dimensions, timing, service definitions, vehicle speed, and initial route reachability are validated before a `Scenario` reaches the engine. This boundary permits future generators, APIs, editors, or experiment runners to construct `Scenario` directly without YAML.

## Determinism

Determinism depends on stable event sequences, ordered resource queues, sorted graph adjacency, explicit A* tie-breaking, and owned seeded randomness. Event sinks and snapshot reads do not participate in scheduling. Tests compare identical-run event histories, final snapshots, and metrics.

## Integrations

- **Unreal Engine 5.8:** `URampLabSimulationSubsystem` consumes snapshots/events; stable Actors mirror entities and visual interpolation samples the authoritative journey timing.
- **ROS 2:** optional native Windows workspace adapts autonomy measurements and commands to messages without importing ROS clocks or dependencies into the core. A separate `rclcpp` controller consumes those messages and publishes `cmd_vel`; see [ros2.md](ros2.md).
- **Experiment runner:** implemented standalone library and CLI build fresh `Scenario` values, execute independent simulations in a bounded pool, and aggregate final metrics without retaining batch event histories.
- **Autonomy simulation:** a separate 20 ms fixed-step subsystem simulates tug dynamics, seeded GNSS/IMU/odometry/LiDAR, A* waypoint following, safety stops, and collision metrics. Its controller sees observations and mission data, never ground truth. Autonomy experiments retain compact mission results only.
- **Turnaround operations:** opt-in aircraft task DAGs are scheduled on simulation time and exposed through snapshots, events, CLI/experiment outputs, ROS, and the Unreal operator panel. See [turnaround operations](turnaround-operations.md) for policies and current boundaries.
- **Network visualization:** server layer serializes snapshots/events outside the core.
- **Record/replay:** stable external schema can later be specified without changing event production.

Parallel execution across operational and autonomy runs is implemented. Parallelism within a single simulation, distributed execution, and ABI stabilization remain later work. ROS 2 remains an optional adapter requiring the separately installed Windows environment described in [ros2.md](ros2.md).
