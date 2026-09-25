# Architecture

## System boundary

Airside Sim's first milestone is a deterministic, single-process, headless discrete-event engine. The simulation library contains domain state and behavior. The CLI is an adapter and may be replaced without changing the engine.

```text
Scenario
    |
    v
Simulation Engine
    |
    +------> Event Scheduler
    |            |
    |            `-- (timestamp, stable sequence)
    |
    +------> Agents / Resources / Routing
    |            |          |
    |            |          `-- AirportGraph -> A* -> Route
    |            `-- FIFO allocation -> vehicle lifecycle
    |
    v
Metrics + Completed State + Event Log
```

## Components

### Core

`SimTime` is a duration, not a calendar timestamp. `EventQueue` assigns a monotonically increasing sequence to each event and orders by `(timestamp, sequence)`. Events may schedule further events while the queue is processing. This makes concurrent service completions predictable and testable.

Strong ID wrappers prevent an aircraft ID from being passed where a gate, vehicle, node, task, or edge ID is required. IDs remain small value types that can cross future API boundaries.

### World and routing

`AirportGraph` owns nodes, undirected road edges, and adjacency. Nodes carry 2D metric coordinates. Edges carry distance, traversal cost, and mutable availability. A* calculates routes only across available edges and returns ordered node/edge IDs, distance, and travel time.

The heuristic multiplies Euclidean distance by the minimum enabled edge cost per meter. That lower bound is admissible even when roads have different traversal costs. Queue and predecessor ties use stable numeric IDs.

### Agents

Aircraft and service vehicles expose explicit guarded transitions. Aircraft own required service-task state and timing. Vehicles record the active/last route and accumulated busy time, but reference assigned aircraft only by strong ID. They do not own or point to aircraft.

### Operations

Each service fleet has a FIFO `ResourcePool`. On aircraft arrival, each required service is requested independently, allowing fuel and baggage work to overlap. If a vehicle is unavailable, the request waits. A released vehicle is immediately assigned to the oldest request after it physically returns to depot.

The `Simulation` coordinates components through scheduled domain events:

```text
AircraftArrival
    -> resource request
    -> vehicle dispatch (A* route)
    -> VehicleArrivalAtAircraft
    -> ServiceCompleted
    -> vehicle return (A* route)
    -> VehicleArrivalAtDepot
    -> next queued assignment
```

When all required tasks complete, departure is scheduled for the later of readiness and scheduled departure. Road-availability changes are ordinary scheduled events carrying an edge ID and availability value.

### Metrics

Metrics are calculated after the event queue drains from completed aircraft and vehicle state. Calculation is separate from workflow mutation, which permits direct unit testing and future alternative report adapters.

## Ownership and performance posture

The simulation owns scenario state by value. There are no singletons, shared pointers, callbacks capturing object lifetimes, or global mutable registries. Lookup collections and event storage allocate in this small milestone; the public concepts leave room for indexed storage, preallocation, and data-oriented processing once profiling justifies them.

The current engine is deliberately single-threaded. Stable event sequencing and isolated component state provide a clear baseline before partitioning or parallel event execution is considered.

## Future integration boundaries

These are planned boundaries, not implementations in this milestone:

- **Unreal Engine:** consume versioned state snapshots and event streams through a separate adapter. Unreal renders and interpolates recorded routes; it does not advance authoritative simulation state.
- **ROS2:** map engine events and snapshots to messages in an adapter target. ROS executors and clocks remain outside the core.
- **Python experiment runner:** invoke a stable C ABI or process-level scenario/result interface for batches. Python is orchestration, not the simulation engine.
- **Sensor simulation:** sample authoritative poses/state in a separate sensor layer so sensor frequency does not dictate the discrete-event clock.
- **Parallel simulation:** first profile and replace lookup/allocation hot spots; later partition independent work while retaining deterministic merge ordering.

## Extension constraints

New presentation or integration targets should depend on `airside_sim`; the library must never depend back on them. External timestamps must be translated at the boundary. Stochastic behavior must draw from owned, seeded random streams and must not depend on container iteration order or thread scheduling.
