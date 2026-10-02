# Aircraft turnaround operations

## Task orchestration

Turnaround orchestration is an opt-in, deterministic extension to the discrete-event simulation. Each aircraft has a scenario-stable turnaround ID, target off-block time, and service-task DAG. Supported tasks are deboarding, fueling, catering, cabin cleaning, baggage unload, baggage load, and pushback preparation. The scheduler starts every eligible independent task whose required resource is available; prerequisites, earliest-start times, service capacity, task state, and timestamps remain part of the core simulation. Existing scenarios without `service_tasks` keep their previous behavior.

The critical-path estimate walks the validated DAG using remaining service durations, prerequisite completion estimates, earliest-start bounds, and dispatched mobile vehicles' expected arrival. It reports estimated ready time, target-off-block slack, critical-path task IDs, and late prediction. Mobile dispatch priority reflects critical-path membership, aircraft/task slack, and deadline urgency; the Goal 13 dispatcher applies queue aging and deterministic route-cost and vehicle-ID tie breaks.

## Shared fleet movement and disruption handling

Fueling, baggage unload, and baggage load are submitted as live `autonomy::FleetSimulation` service requests. The Goal 13 dispatcher assigns capable vehicles, and each request follows the shared airport graph, fixed-step controller, route reservations, collision checks, road availability, and safe-stop behavior. Core task state follows queued, assigned, en-route, servicing, completed, reassigned, and failed transitions. Vehicle snapshots include live fleet pose and dispatch status.

Scenarios may define baggage staging roads by naming a destination `<Gate Name> Baggage Stand`; baggage tasks route to that stand so the cart does not occupy the aircraft's fueling point. `turnaround_disrupted.yaml` changes baggage-load duration while the task is queued, then safely takes BaggageCart-1 out of service during baggage unload. The dispatcher requeues that request to BaggageCart-Backup. The duration change updates the readiness estimate without resetting state; work already in service remains non-preemptible. Unavailable vehicles and failed requests are reflected in task/turnaround state and structured events.

## Scenarios and exported evidence

- `turnaround_normal.yaml` runs one full DAG with independent crew and vehicle work in parallel.
- `turnaround_contention.yaml` runs two aircraft against one fuel truck, one baggage cart, and finite-capacity crews.
- `turnaround_disrupted.yaml` combines a queued-task duration increase with a live vehicle outage and backup reassignment.

The CLI prints turnaround/task state, critical path, slack, wait, replan, reassignment, collision, minimum-separation, reservation, created/completed/failed fleet-request, and unresolved-request metrics. Mobile fleet utilization is busy time, including route travel and servicing, divided by elapsed simulation time and available vehicles. `--record-events FILE` writes ordered event JSONL; `--metrics-json FILE` and `--metrics-csv FILE` write fleet safety/request and per-task timing data. Experiment CSV/JSON outputs also carry turnaround and fleet metrics. Sample outputs are regenerated under the ignored `results/turnaround_validation/` directory.

Snapshots and ordered structured events feed `/ramplab/turnaround/state` and `/ramplab/turnaround/events` in ROS and the Unreal operations panel. The panel shows turnaround/task progress, deadlines and slack, critical-path tasks, vehicle assignment, fleet position/status, and failure/reassignment information.

## Validation and boundaries

Release C++ validation passed 160/160 tests; the five turnaround tests include dependency overlap, deterministic contention, duration replan, successful outage reassignment, and an unrecoverable outage that fails safely without departure. Seed 42 scenario runs completed 1/1 normal, 2/2 contention, and 1/1 disrupted turnarounds. They reported zero collisions, minimum separations of 8.0 m, 2.1 m, and 2.6 m respectively, no outstanding reservations or unfinished requests, and exactly one reassignment in the backup scenario. Their JSON parsed and CSV exports contained 7, 12, and 7 task rows.

The ROS bridge built in the configured Pixi/colcon environment; its suite passed 19/19 tests. The live topic probe observed the normal turnaround depart with all 7/7 tasks complete and 21 ordered events. The Unreal Editor target built successfully, and a visible windowed Goal 14 validation run reported 1/1 complete, no failures or reassignments, zero collisions, 8.0 m minimum separation, six reservation requests, three contentions, and zero outstanding/unresolved requests. Its Recent Events panel log ended with ready-for-departure and aircraft departure.

These are deterministic software-simulation results using synthetic airport geometry, service durations, and resource policies. They do not establish real-airport performance, ROS-controlled dispatch, physical-vehicle safety, or production acceptance.
