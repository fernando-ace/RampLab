# Aircraft turnaround operations

## Task orchestration

Turnaround orchestration is an opt-in, deterministic extension to the discrete-event simulation. Each aircraft has a scenario-stable turnaround ID, target off-block time, and service-task DAG. Supported tasks are deboarding, fueling, catering, cabin cleaning, baggage unload, baggage load, and pushback preparation. The scheduler starts every eligible independent task whose required resource is available; prerequisites, earliest-start times, service capacity, task state, and timestamps remain part of the core simulation. Existing scenarios without `service_tasks` keep their previous behavior.

The critical-path estimate walks the validated DAG using remaining service durations, prerequisite completion estimates, earliest-start bounds, and dispatched mobile vehicles' expected arrival. It reports estimated ready time, target-off-block slack, critical-path task IDs, and late prediction. Mobile dispatch priority reflects critical-path membership, aircraft/task slack, and deadline urgency; the Goal 13 dispatcher applies queue aging and deterministic route-cost and vehicle-ID tie breaks.

## Shared fleet movement and disruption handling

Fueling, baggage unload, and baggage load are submitted as live `autonomy::FleetSimulation` service requests. The Goal 13 dispatcher assigns capable vehicles, and each request follows the shared airport graph, fixed-step controller, route reservations, collision checks, road availability, and safe-stop behavior. Core task state follows queued, assigned, en-route, servicing, completed, reassigned, and failed transitions. Vehicle snapshots include live fleet pose and dispatch status.

Scenarios may define baggage staging roads by naming a destination `<Gate Name> Baggage Stand`; baggage tasks route to that stand so the cart does not occupy the aircraft's fueling point. `turnaround_disrupted.yaml` changes baggage-load duration while the task is queued, then safely takes BaggageCart-1 out of service during baggage unload. The dispatcher requeues that request to BaggageCart-Backup. The duration change updates the readiness estimate without resetting state; work already in service remains non-preemptible. Unavailable vehicles and failed requests are reflected in task/turnaround state and structured events.

An optional `outage_safe_node` on a fleet vehicle names a refuge for an outaged vehicle. The fleet releases its active assignment, deterministically requeues affected work, and routes the vehicle to that node under shared reservations before marking it unavailable. Without a reachable refuge, the vehicle performs a controlled stop where it is; it remains physically present and collision-relevant. Approaching vehicles yield on a projected hard-clearance conflict with an unavailable vehicle. The collision radius and collision counter are unchanged.

## Scenarios and exported evidence

- `turnaround_normal.yaml` runs one full DAG with independent crew and vehicle work in parallel.
- `turnaround_contention.yaml` runs two aircraft against one fuel truck, one baggage cart, and finite-capacity crews.
- `turnaround_disrupted.yaml` combines a queued-task duration increase with a live vehicle outage and backup reassignment.

The CLI prints turnaround/task state, critical path, slack, wait, replan, reassignment, collision, minimum-separation, reservation, created/completed/failed fleet-request, and unresolved-request metrics. Mobile fleet utilization is busy time, including route travel and servicing, divided by elapsed simulation time and available vehicles. `--record-events FILE` writes ordered event JSONL; `--metrics-json FILE` and `--metrics-csv FILE` write fleet safety/request and per-task timing data. Experiment CSV/JSON outputs also carry turnaround and fleet metrics. Sample outputs are regenerated under the ignored `results/turnaround_validation/` directory.

Snapshots and ordered structured events feed `/ramplab/turnaround/state` and `/ramplab/turnaround/events` in ROS and the Unreal operations panel. The panel shows turnaround/task progress, deadlines and slack, critical-path tasks, vehicle assignment, fleet position/status, and failure/reassignment information.

## Validation and boundaries

Release C++ validation passed 163/163 tests; focused turnaround coverage includes dependency overlap, deterministic contention, duration replan, safe vehicle outage/reassignment, and an unrecoverable outage that fails safely without departure. Seed 42 scenario runs completed 1/1 normal, 2/2 contention, and 1/1 single-aircraft disrupted turnarounds. They reported zero collisions, no outstanding reservations or unfinished requests, and exactly one reassignment in the backup scenario. Their JSON parsed and CSV exports contained 7, 12, and 7 task rows.

The ROS bridge built in the configured Pixi/colcon environment; its suite passed 19/19 tests. The live topic probe observed the normal turnaround depart with all 7/7 tasks complete and 21 ordered events. The Unreal Editor target built successfully, and a visible windowed Goal 14 validation run reported 1/1 complete, no failures or reassignments, zero collisions, 8.0 m minimum separation, six reservation requests, three contentions, and zero outstanding/unresolved requests. Its Recent Events panel log ended with ready-for-departure and aircraft departure.

These are deterministic software-simulation results using synthetic airport geometry, service durations, and resource policies. They do not establish real-airport performance, ROS-controlled dispatch, physical-vehicle safety, or production acceptance.

## Multi-aircraft flight bank

Goal 15 extends the existing `Simulation` instance rather than creating a second execution path. Every aircraft retains its own typed aircraft ID, turnaround ID, gate, schedule, task DAG, task timing, readiness, departure, and failure state. Tasks are assigned globally unique `TaskId` values while prerequisite references remain scoped to their owning aircraft. The simulator advances all arrivals, task transitions, vehicle movements, and departures through its deterministic event queue.

`turnaround_flight_bank.yaml` demonstrates three aircraft at gates A1, A2, and A3 with staggered arrivals and departure windows. The single shared fueling vehicle and baggage vehicle serve all three through the existing Goal 13 dispatcher and Goal 11 road reservations. Queue eligibility is checked before task priority; the dispatcher then considers critical-path/deadline urgency, queue aging, route cost, and stable IDs. Gate occupancy is recorded in snapshots and released only on departure; overlapping arrivals at an occupied gate fail safely rather than allowing double occupancy.

Each aircraft's task list is a validated dependency graph. The flight-bank baggage chain is `baggage unload -> baggage load -> pushback preparation`; fueling can proceed independently. Completed work is terminal and each request has one dispatcher assignment at a time. Dispatch, requeue, outage, task lifecycle, readiness, departure, and failure records retain simulation timestamps and entity IDs in the existing event stream.

The CLI JSON includes aircraft/gate identity, scheduled and actual arrival/departure, schedule delay, completed/unfinished task totals, per-task state/timing, and aggregate fleet/request/safety metrics. CSV remains one row per service task and now repeats its aircraft and departure summary columns. JSONL retains the ordered operational event stream. Run the flight bank and write all artifacts with:

```powershell
.\build-final-msvc\Release\airside_cli.exe --scenario scenarios\turnaround_flight_bank.yaml --seed 42 --quiet `
  --metrics-json results\goal15\flight-bank.json --metrics-csv results\goal15\flight-bank.csv `
  --record-events results\goal15\flight-bank.jsonl
```

For three-aircraft contention, `turnaround_contention.yaml` uses the same three gates with one vehicle per mobile service type and earlier schedules. `turnaround_flight_bank_disrupted.yaml` raises AX101's queued baggage-load duration during the operation. The canonical `turnaround_flight_bank_outage.yaml` adds an outage at simulation time 130 while AX101's baggage-unload request is assigned, and names a safe bay for the failed cart. The dispatcher reassigns task 3 to BaggageCart-Backup at time 130; it starts at 168 and completes at 408. Seed 42 completes all 18 required tasks and all three departures with zero collisions, 2.111 m minimum separation, no unfinished requests, and no outstanding reservations. Against the same one-cart flight-bank and task-duration-disruption control, total departure delay rises by 73 simulated seconds (from 113 to 186 seconds across the three aircraft). This is a simulation comparison, not a real-airport performance estimate.

Repeatable serial and parallel flight-bank experiments are defined in `experiments/turnaround_goal15.yaml`. Compare the complete simulation columns (excluding `execution_ms`, which is wall-clock measurement) with:

```powershell
.\build-final-msvc\Release\airside_experiment.exe --experiment experiments\turnaround_goal15.yaml --workers 1 --quiet --output results\goal15\serial
.\build-final-msvc\Release\airside_experiment.exe --experiment experiments\turnaround_goal15.yaml --workers 4 --quiet --output results\goal15\parallel
```

The canonical outage run with seed 42 produced event digest `5728417370311899710`. Across seeds 42–44, the three-run serial and four-worker output rows match across all simulation metrics, per-aircraft completion/task ordering, and fleet outcomes (excluding wall-clock `execution_ms`).

The ROS observer remains read-only. `/ramplab/turnaround/state` identifies each aircraft, gate, task state, assignments, and departure schedule; it now also carries actual arrival/departure and task counts. Ordered `/ramplab/turnaround/events` records include gate IDs. Use `--min-aircraft 3` with `verify_turnaround_topics.py` when probing the flight-bank scenario.

The Unreal turnaround validation flag selects `turnaround_flight_bank_outage`; its operations panel iterates aircraft and turnaround snapshots, showing gate, task progress, scheduled/actual departure, and live schedule delay. The validation log includes aggregate completion, reassignment, request, reservation, and collision results plus Recent Events. This is an operations visualization over core snapshots; it does not move dispatch authority into Unreal.

Turnaround `departure_delay` is measured from actual departure against scheduled departure. The target off-block time remains a separate readiness deadline used for slack and late prediction. This distinction keeps forecast slack separate from realized schedule delay.

All turnaround durations, gates, fleet capacity, road geometry, deadlines, and disruptions in these scenarios are synthetic. They are not validated estimates of airport throughput, departure delays, apron safety, staffing, or ground-handler performance.
