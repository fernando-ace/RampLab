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
                 /             \
                /               \
               v                 v
      Structured Events      Snapshot API
               |                 |
               v                 v
          Consumers          Consumers
```

- **Simulation engine:** authoritative mutable domain model.
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

airside_cli
  argument parsing, event sinks, snapshot diagnostics, report formatting
```

## Execution model

Construction validates core duration invariants and schedules scenario events. `advance()` removes exactly one internal scheduled event, moves `SimTime`, mutates authoritative state, schedules consequences, and emits zero or more public event records. `finished()` reports an empty scheduler. `run()` is a convenience loop over `advance()`.

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

An adapter owns the world transform. For Unreal, it will normally scale meters to centimeters, choose which Unreal horizontal axes correspond to east/north, and supply elevation separately. No Unreal coordinate or math type enters the core.

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

## Future integrations

- **Unreal Engine:** plugin adapter consumes snapshots/events; Actors mirror entities.
- **ROS2:** adapter maps records and snapshots to messages without importing ROS clocks into the core.
- **Experiment runner:** process or C ABI builds `Scenario` values and consumes metrics.
- **Network visualization:** server layer serializes snapshots/events outside the core.
- **Record/replay:** stable external schema can later be specified without changing event production.

Parallel execution, sensor simulation, continuous motion, and ABI stabilization are later work and are not implemented here.
