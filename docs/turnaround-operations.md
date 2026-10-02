# Aircraft turnaround operations

## Authority and model

Turnaround orchestration is an opt-in extension to the existing discrete-event simulation. YAML task definitions are validated by `airside_scenario`; the core owns task state, dependency unlocking, resources, timing, readiness, estimates, metrics, and events. Existing scenarios without `service_tasks` preserve their Goal 1–13 behavior.

Each configured aircraft has a turnaround ID, gate, scheduled times, target off-block time, operational state, and service tasks. Task IDs are scenario-stable values. A task records service type, deterministic duration, prerequisite IDs, earliest start, optional latest desirable completion, state, resource assignment, and request/start/completion timestamps. Loader validation rejects duplicate service types within one turnaround, missing/self prerequisites, cyclic graphs, negative times, and nonpositive durations.

The supported task kinds are deboarding, fueling, catering, cabin cleaning, baggage unload, baggage load, and pushback preparation. Prerequisite lists define the DAG. The scheduler scans aircraft and each task list in scenario order, emits readiness in that order, and starts every eligible task whose resource is available. It does not impose a single turnaround-wide lock.

## Resource and movement model

Fueling and baggage unload use the existing mobile `ServiceVehicle`, graph router, vehicle lifecycle, and resource pools. They travel from the depot to the occupied aircraft gate, service for the task duration, then return to the depot. No mobile vehicle is teleported. Abstract deboarding, catering, cleaning, baggage-load, and pushback tasks use deterministic finite-capacity crews; each abstract type defaults to one crew. Abstract crew IDs use the lowest available numeric slot.

This operational layer uses the discrete-event engine's existing `ResourcePool`. It does not adapt `autonomy::FleetSimulation`'s intersection reservations or collision-avoidance simulation. A mobile task's gate is represented by the aircraft's existing gate occupancy; there is no separate apron service-stand reservation in this version.

Queued mobile requests use this deterministic priority policy:

1. Add 10 points when the request's task is on the current estimated critical path.
2. Add 20 points at zero or negative schedule slack, 10 points at up to five minutes of slack, or 5 points at up to ten minutes of slack.
3. Add one aging point for every 60 seconds of simulation-time waiting.
4. Break ties by earlier request time, then stable request insertion sequence.

Immediate assignments remain immediate. Existing non-turnaround requests pass priority zero and retain FIFO ordering. Priority affects allocation only; it never bypasses routing or vehicle state checks.

## Estimate and disruption

The critical-path estimate walks the validated DAG in stable task order. It uses remaining task durations, prerequisite finish estimates, earliest-start bounds, and a dispatched mobile vehicle's expected gate-arrival time. It reports estimated ready time, target-off-block slack, a task-ID path, and a predicted-late event. It is a scheduling estimate, not an airline performance model.

`turnaround_disrupted.yaml` deterministically increases the duration of a blocked baggage-load task while its turnaround is active. The coordinator updates its estimate without resetting simulation state. A duration change aimed at an in-progress or completed service is rejected: service work is non-preemptible. Vehicle-unavailability injection and recovery through Goal 13's autonomy fleet dispatcher are not implemented in this version.

## Scenarios and outputs

- `turnaround_normal.yaml` runs one complete DAG and overlaps independent work.
- `turnaround_contention.yaml` runs two turnarounds against one fuel truck, baggage cart, and one crew per abstract service type.
- `turnaround_disrupted.yaml` demonstrates a duration disruption and replan.

The snapshots expose turnaround and task state. Structured events include creation, readiness, dispatch, start, completion, disruption, critical-path changes, late prediction, and ready-for-departure. The CLI prints per-turnaround task timing, critical path, slack, wait, replan, and priority metrics and writes the same event schema to JSON Lines. Experiment `runs.csv` appends turnaround aggregates; `experiment.json` stores the individual turnaround completion, delay, estimate, slack, and critical-path IDs.

The ROS observer publishes `/ramplab/turnaround/state` and `/ramplab/turnaround/events`; Unreal reads the same core snapshot and events and shows status, estimates, active services, resource assignments, and critical-path tasks.

Service times, abstract resource capacities, deadlines, and disruption values in the sample files are synthetic simulator assumptions. RampLab does not claim to reproduce real airport performance.

## Current limitations

- Failure and timeout states are represented in the integration schema, but the discrete-event turnaround scenarios currently complete or throw validation/runtime errors; there is no safe-timeout policy.
- Task reassignments are counted but not exercised; vehicle outage/reassignment is pending integration with the Goal 13 fleet dispatcher.
- Road and gate reservation safety remains limited to the existing operational core; the Goal 13 autonomy traffic reservation system is not yet shared with these vehicles.
- Latest-desirable completion timestamps are retained as task metadata but do not currently change allocation priority.
- Critical-path calculations ignore downstream route congestion and use current journey arrival for dispatched mobile tasks.
