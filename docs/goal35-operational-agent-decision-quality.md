# Goal 35: Operational Agent Decision Quality

## Objective and scope

Goal 35 asks whether deterministic ADAPTIVE aircraft routing adds operational value when more than one safe route or a wait action is available. The experiment preserves the Goal 34 SCRIPTED policy and lets the result show parity, a win, or a loss. All newly created scenarios are simulator fixtures; none of the findings are claims about real airport performance.

## Architecture

The existing `DecisionPolicy` interface remains the policy boundary. The simulator creates a read-only `Observation` containing the current route, destination, closure and reservation state, aircraft speed, and a bounded list of route candidates. `ScriptedRoutingPolicy` continues to use the legacy shortest available route planner and does not use candidate scores. `AdaptiveRoutingPolicy` evaluates the candidates, proposes a route or waits, and returns through the existing route application path.

`find_route_candidates` enumerates up to six simple, available paths. Enumeration is bounded to 256 explored paths and 32 edges per path. It honors one-way restrictions and closures, sorts by planner time, distance, then node sequence, and removes duplicate node sequences. It is used to compare operations; it does not grant movement permission.

The observation estimates reservation waiting from current hard reservations and other active agents' selected route intentions. An active edge uses the observed remaining phase time with a 30 second minimum estimate. A planned route edge estimates occupancy from configured aircraft speed and adds pushback duration if that aircraft is still waiting for pushback. When an aircraft is blocked, the observation also includes a `WAIT` candidate. A current route invalidated by closure remains visible as a rejected candidate, while the alternate-path enumerator omits unavailable edges.

## Decision model

At each route-selection or blockage decision point, the trace records simulation time, agent and objective IDs, a structured observation, every candidate's route and predicted costs, the selected candidate, action, reason, and explanation. Candidate kinds are:

- `CONTINUE_CURRENT_ROUTE`
- `ALTERNATE_ROUTE`
- `WAIT`

For a route candidate, predicted taxi time is `ceil(route distance / aircraft speed)`. The deterministic route score is:

```text
score_seconds = predicted_taxi_seconds + expected_reservation_wait_seconds + distance_m / 50
```

The final term is a small distance penalty expressed in seconds. The policy chooses the lowest score; exact ties retain the first candidate in deterministic candidate order. For a new objective with no current route, it selects the lowest-scoring available route. For a live route, it only changes to a materially better alternate after the configured 30 second improvement threshold and 10 second decision cooldown. A wait candidate includes the current reservation estimate plus the remaining current-route taxi time and distance penalty, so it is compared against alternate routes on the same trip-completion estimate. The policy does not use machine learning, external services, randomness, or future simulator events.

The present observation does not contain supported gate reassignment, service-vehicle traffic, turnaround deadlines, or nonzero aircraft priority data, so those factors are not fabricated or scored. The experiment measures departure delay and queueing after execution rather than predicting downstream turnaround risk.

## Safety boundary

Safety is not a score term. Candidate paths use currently available graph edges. Existing route validation, edge and node reservations, closures, collision checks, separation rules, runway occupancy, and safe-failure behavior remain authoritative when a proposed route is applied and executed. The adaptive policy cannot mutate reservations or proceed through a closed or reserved movement. The Goal 34 closure and operator-intervention paths remain in place.

All experiment runs completed with zero surface collisions and zero surface safe failures. Minimum aircraft separation was recorded for every matrix run; the value is zero only where the simulator has no separation sample, and is otherwise reported as measured.

## Fair comparison and experiment matrix

Each paired run uses the same source scenario, seed, aircraft demand, schedule, disruptions, geometry, and simulator settings. The only scenario difference is `surface_operations.aircraft_routing_policy`. The SCRIPTED baseline retains its existing planner behavior.

The matrix uses seeds 42, 43, and 44 across six fixtures:

| Scenario label | Source fixture | Coverage |
|---|---|---|
| `normal_operations` | `turnaround_lifecycle.yaml` | Normal arrival, turnaround, departure, and runway operations |
| `congestion` | `surface_traffic.yaml` | Three competing aircraft and surface/runway contention |
| `temporary_route_disruption` | `surface_traffic_disrupted.yaml` | Scheduled taxiway closure |
| `competing_aircraft_demand` | `mixed_runway_operations.yaml` | Arrivals and departures competing for runway and surface resources |
| `combined_stress` | `mixed_runway_disrupted.yaml` | Mixed demand with a route disruption |
| `adaptive_route_choice_demo` | `goal35_route_choice_demo.yaml` | Short shared corridor versus longer direct path |

This produced 36 serial policy runs and 36 parallel replays. For each scenario, seed, and policy, the normalized metrics and SHA-256 hashes for metrics, event stream, and decision trace matched between serial and parallel execution. The exact source and configured scenario hashes are in [experiment_config.json](goal35-evidence/config/experiment_config.json); the run index and result digest ledger are in [experiment.json](goal35-evidence/experiment.json).

## Results

Values below are means across seeds 42–44. Taxi time and distance are departure taxi totals; operational waiting is total surface waiting. The completion column reports completed/failed agent objectives across all runs in that scenario-policy group. Separation is the minimum measured value across the three seeds.

| Scenario | Policy | Mean departure delay (s) | Departure taxi time (s) | Taxi distance (m) | Surface wait (s) | Objectives complete/failed | Replans / decisions | Collisions / safety failures | Minimum separation (m) |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Normal operations | SCRIPTED | 1722.3 | 1535 | 1535 | 553 | 15 / 0 | 6 / 39 | 0 / 0 | 30.0 |
| Normal operations | ADAPTIVE | 1722.3 | 1535 | 1535 | 553 | 15 / 0 | 6 / 39 | 0 / 0 | 30.0 |
| Congestion | SCRIPTED | 606.7 | 735 | 672 | 105 | 9 / 0 | 0 / 27 | 0 / 0 | 30.0 |
| Congestion | ADAPTIVE | 606.7 | 735 | 672 | 105 | 9 / 0 | 0 / 36 | 0 / 0 | 30.0 |
| Temporary route disruption | SCRIPTED | 625.1 | 800.3 | 768 | 64.3 | 9 / 0 | 3 / 30 | 0 / 0 | 38.6 |
| Temporary route disruption | ADAPTIVE | 625.1 | 800.3 | 768 | 64.3 | 9 / 0 | 3 / 36 | 0 / 0 | 38.6 |
| Competing aircraft demand | SCRIPTED | 898.3 | 1620 | 672 | 1137.3 | 15 / 0 | 0 / 39 | 0 / 0 | 18.6 |
| Competing aircraft demand | ADAPTIVE | 898.3 | 1620 | 672 | 1137.3 | 15 / 0 | 0 / 90 | 0 / 0 | 18.6 |
| Combined stress | SCRIPTED | 898.3 | 1620 | 672 | 1137.3 | 15 / 0 | 0 / 39 | 0 / 0 | 18.6 |
| Combined stress | ADAPTIVE | 898.3 | 1620 | 672 | 1137.3 | 15 / 0 | 0 / 90 | 0 / 0 | 18.6 |
| Route-choice demo | SCRIPTED | 698.7 | 941 | 780 | 273 | 9 / 0 | 0 / 27 | 0 / 0 | 30.0 |
| Route-choice demo | ADAPTIVE | 797.7 | 1267 | 1056 | 294 | 9 / 0 | 15 / 45 | 0 / 0 | 26.1 |

In the five existing operational fixtures, reported operational metrics match across policies; ADAPTIVE records more decisions where its wait/re-evaluation behavior is active. The route-choice demonstration is scenario-dependent and makes the aggregate result worse for ADAPTIVE: across three seeds, its mean departure delay is 99 seconds higher, departure taxi time is 326 seconds higher, taxi distance is 276 metres higher, and surface wait is 21 seconds higher. Both policies complete all objectives with zero failures or safety findings.

The fixture still shows a local benefit for one decision. In seed 42, aircraft 3 predicts 211 seconds of waiting on the shorter shared path. ADAPTIVE selects the direct 280 metre route with score 285.6; SCRIPTED selects the 200 metre shared route because its planner cost is 20 seconds. Aircraft 3 completes its route objective at second 858 under ADAPTIVE and 909 under SCRIPTED, 51 seconds earlier in that paired run. Other aircraft absorb downstream delay, so that local improvement does not improve the total scenario result. These are controlled simulator outcomes from synthetic fixture inputs, not airport observations.

## Representative decision trace

At simulation second 548, aircraft 3 has no active route and is deciding how to reach node 2:

| Candidate | Distance | Predicted taxi | Expected wait | Score | Decision |
|---|---:|---:|---:|---:|---|
| Direct route `[6, 2]` | 280 m | 280 s | 0 s | 285.6 s | ADAPTIVE selected |
| Shared corridor `[6, 7, 2]` | 200 m | 200 s | 211 s | 415.0 s | Valid alternative |

The structured observation and candidate records are preserved in [the ADAPTIVE decision trace](goal35-evidence/runs/adaptive_route_choice_demo/seed-42/adaptive/decisions.json), at event `agent-decision-2`. The paired SCRIPTED trace at the same event is [here](goal35-evidence/runs/adaptive_route_choice_demo/seed-42/scripted/decisions.json). Metrics and native event streams for both policies are in the adjacent run folders.

The wait prediction is an estimate from the route intent and current reservation state. It is not a guarantee that the full predicted wait would occur. The evidence includes both the prediction and the realized outcome so reviewers can inspect forecast error.

## Validation

- MSVC Release configure and build of `airside_cli` and `airside_tests`: passed.
- Release CTest: 202/202 passed.
- Goal 34 policy and surface regressions, including the new route-choice outcome test: passed.
- Scenario Studio/dashboard and Goal 33 live-control Python suite: 73 ran, 72 passed, 1 skipped. The five Goal 33/live-control cases all passed after placing the Release CLI at the path expected by the integration suite.
- Goal 19 experiment analysis suite: 10 passed.
- Evidence-bundle suite: 13 passed.
- Airport ingestion suite: 13 passed.
- Airport scenario-generation suites: 31 passed.
- Python compilation for the Goal 35 matrix runner: passed.
- Deterministic paired replay and serial/parallel normalized-result plus artifact-hash comparisons: 36/36 passed.
- JavaScript syntax checks were not applicable because Goal 35 changes no JavaScript files.
- `git diff --check`: recorded after final source and evidence updates.

The first unittest discovery attempts for packages with relative imports were invoked as top-level modules and failed during import. Re-running those same suites by package-qualified module names succeeded; no tests or product code were changed to address invocation context. The live-control suite initially skipped because it expects `build-goal34/Release/airside_cli.exe`; the built Goal 35 Release CLI was copied to that ignored path in this worktree and all five cases then passed.

## Limitations

- The scoring weights and 50 metre distance conversion are fixed engineering choices, not calibrated operational values.
- Reservation forecasts are deterministic estimates from current phase timing and active route intentions; they do not model future events or probability distributions.
- Candidate enumeration is bounded and does not prove it found every simple route in an unusually dense graph.
- Gate reassignment, service-vehicle congestion, turnaround deadlines, and differentiated aircraft priorities are outside the current aircraft observation model.
- Three seeds provide a reproducible comparison, not evidence of broad real-world generalization.
- ADAPTIVE does not outperform SCRIPTED in the aggregate demonstration; the route-choice fixture exposes a local benefit and a larger total cost.

## Evidence and reproduction

Re-run the matrix from the repository root with:

```powershell
python tools/goal35/run_matrix.py --cli build-goal35-msvc/Release/airside_cli.exe --workers 4
```

The script writes the configured scenario copies, per-run metrics, native event streams, structured decision traces, matrix CSV, run manifest, serial/parallel replay verification, and hashes under [docs/goal35-evidence](goal35-evidence/README.md). The evidence is reproducible from the checked-in synthetic scenarios and configured seeds; it contains no real-airport efficiency claim.
