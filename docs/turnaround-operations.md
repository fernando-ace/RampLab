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

## Surface traffic operations (Goal 16)

`surface_operations` opts a turnaround scenario into explicit gate pushback, taxi, runway-queue, and departure phases. It names a departure handoff node in the existing airport graph and sets synthetic pushback duration, runway occupancy, and aircraft speed. A* remains deterministic and follows currently enabled graph edges. A traversing aircraft holds its taxi edge and reserves the next node; the aircraft keeps the edge commitment until reaching the next node. Pushback reserves its first connector so a blocked connector defers pushback. Once at the departure handoff, aircraft queue FIFO by queue-arrival time and stable aircraft ID, and one aircraft occupies the simplified runway resource at a time.

Airport graph edges are two-way by default. Set `one_way: true` on an edge to restrict A* routes to its configured `from` → `to` direction; dynamic closures still disable that edge for both routing and traversal. Taxi edges and intersections have single-aircraft reservations, while the simplified runway has capacity one.

When a closure invalidates an uncommitted part of a route, the aircraft replans from its next safe node. A committed edge finishes before the aircraft replans. If no path remains, the aircraft stays safely stopped and does not depart. These rules reuse `AirportGraph`, A*, edge availability events, snapshots, and the existing ordered event stream. The surface edge reservation is a coarse single-aircraft resource; it is not a surveyed airport movement-area model.

The deterministic demonstration fixtures are `surface_traffic.yaml` and `surface_traffic_disrupted.yaml`; seed 42 is the reference seed. The disrupted scenario closes the north departure link while an aircraft is approaching it and includes a south connector as an alternate. Surface metrics are included in CLI JSON/CSV and experiment run exports, including departures per simulated hour; pushback, route, wait, reservation, closure, reroute, queue, clearance, departure, and safe-failure records are available in JSONL. A CTest export check parses control/disruption JSON, verifies CSV metric/timing columns, checks ordered lifecycle events, and compares repeated seed-42 JSONL byte-for-byte. The ROS turnaround observer serializes current aircraft position, heading, speed, surface state, route, wait reason, reroutes, and taxi distance; the Unreal viewer reads the same snapshot and has control-panel entries for the two fixtures.

Release seed-42 control completed all three turnarounds and surface departures. It produced two surface waits totaling 95 seconds, 735 taxi seconds, 10 runway-queue seconds, and zero surface failures or aircraft collisions. Its elapsed-simulation throughput was 8.845 departures per hour. Minimum sampled spacing was 30.000 m aircraft-to-aircraft and 80.083 m aircraft-to-ground-vehicle. The closure run rerouted once, completed all three departures with zero safe failures and collisions, and delayed AX202 by 55 seconds versus control. Across the flight bank, mean departure delay rose from 607.0 to 625.333 seconds. The three-seed (42–44) experiment matched between one and three workers for all compared surface metrics. These values are synthetic scenario outputs.

The positions and speeds are synthetic. At each 1-second surface update the coordinator samples aircraft-aircraft and aircraft-ground-vehicle spacing; it safely holds affected aircraft below synthetic 20 m aircraft-aircraft and 30 m aircraft-ground hold buffers, without changing the 12 m and 8 m collision counters. The sample cadence and planar geometry do not prove swept-path separation between updates, and intersection geometry is not surveyed. These limitations prevent treating this implementation as operational airport safety or capacity evidence.

The Unreal 5.8 Editor target rebuilt successfully with the Goal 16 core libraries. Visible accelerated windowed control and closure runs both completed: control departed 3/3 with two waits (95 s), 735 taxi seconds over 672 m, 10 s runway queue, zero failures/collisions, and minimum sampled spacing of 30.000 m aircraft-aircraft and 80.083 m aircraft-ground. The closure run departed 3/3 with one reroute, two waits (64 s), 800 taxi seconds over 768 m, zero failures/collisions, and minimum spacing of 38.588 m and 80.083 m. Runtime logs report these metrics from the C++ result; the operations panel presents the same snapshot/result data.

The live ROS 2 probe passed for both fixtures: control observed three departed aircraft, 18 task completions, three pushbacks, three runway queue entries, one traffic wait, and no reroute; closure observed the same departures/tasks/pushbacks/queue entries, one traffic wait, and one reroute. The rebuilt bridge suite passed 19 tests. CLI JSON and CSV parsed with all aircraft, timing, throughput, separation, collision, and queue fields; control JSONL contained 144 ordered events with unique sequence numbers. The dedicated C++ Release suite passed 171 tests (170 GTests and the CLI export integration test), including seed 42–44 serial/parallel metric equality and Goal 15 regressions.
