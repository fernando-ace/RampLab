# Goal 41: KAUO airport operations vertical

## Operation

This vertical runs three illustrative aircraft through arrivals, turnarounds and departures, with nine configured ground units and 33 service tasks. The control and disruption scenarios use the same seed (42), ADAPTIVE aircraft routing policy, source inputs and initial scenario state. At 241 seconds, the disruption case extends Flight KAUO-G31-GA-03 task `G31-03-PUSH` from 180 to 480 seconds. This controlled task-duration event causes the simulator to recompute turnaround readiness.

The C++ RampLab simulator remains authoritative for aircraft, fleet, task, runway and safety outcomes. Agent decisions are exported from its structured decision trace and summarized for operators. Evidence retains native metrics, aircraft rows, fleet assignments, events, full decision traces, repeat runs and the static comparison bundle.

The COUNTERFACTUAL control run exceeded its 1800-second timeout without producing metrics. ADAPTIVE was selected as the strongest practical policy for this integrated run. This is a runtime-based selection; no comparative performance advantage is claimed. The timeout and selection rationale are recorded in `policy-selection.json`.

## Source and assumptions

- The runway reference, dimensions and runway-end positions are FAA source-backed static facts.
- Taxiway and apron geometry is chart-informed and approximate.
- The aircraft schedules and archetypes, parking positions, ground-unit fleet, task requirements and service durations are illustrative simulation assumptions.
- The 300-second pushback service extension is an illustrative scenario assumption. It does not describe a reported, planned or current KAUO condition.
- Ground-unit task assignments are derived from simulator task records. Final vehicle positions are unavailable in the exported metrics and are labeled accordingly.

The ingestion package, provenance manifest, source hashes, canonical input and generated scenario manifests are included in the portable evidence archive.

## Measured results

| Measure | Control | Disruption | Change |
|---|---:|---:|---:|
| Completed turnarounds | 3 | 3 | 0 |
| Arrivals completed | 3 | 3 | 0 |
| Departures completed | 3 | 3 | 0 |
| Required service tasks | 33 | 33 | 0 |
| Taxi time | 1188 s | 1182 s | -6 s |
| Taxi distance | 5877.353 m | 5877.353 m | 0.0 m |
| Surface wait | 1129 s | 1123 s | -6 s |
| Departure delay | 643.0 s | 937.0 s | 294.0 s |
| Reroutes | 0 | 0 | 0 |
| Task reassignments | 0 | 0 | 0 |
| Minimum aircraft separation | 151.030987 m | 948.133186 m | 797.102199 m |
| Minimum aircraft-ground separation | 13.565011 m | 31.549956 m | 17.984945000000003 m |
| Minimum ground-unit separation | 1.969527 m | 1.969527 m | 0.0 m |

The simulator reported 0 aircraft-aircraft, 0 aircraft-ground and 0 ground-fleet collisions for the disruption run. Safety failures and failed objectives are separately recorded in `vertical.json`; these simulator results are evidence for the configured scenario only, not an airport safety determination.

## Autonomous operation and replay

- At 241 s, KAUO-G31-GA-03 pushback service was extended by 300 seconds. A controlled service-delay event changed the planned pushback task from 180 to 480 seconds. The simulator recomputed turnaround readiness.

The full event streams are retained in order. The report does not reconstruct unrecorded aircraft or vehicle motion. Repeat control and repeat disruption runs were checked by the Goal 19 determinism utility; statuses are recorded in `vertical.json` and `determinism.json`.

## Validation

- Data ingestion: PASS.
- Scenario generation: PASS.
- Matched initial state: PASS.
- Scenario completion and exported safety fields: PASS.
- Repeat-run determinism: PASS.
- Release CTest: 218/218 passed.
- Focused Python validation: 70 tests passed; 6 were skipped by existing test conditions.
- Unreal Editor build was attempted with UE 5.8 and could not proceed because the enabled CesiumForUnreal plugin is not installed on this workstation.

## Portable evidence

`goal41-portable-evidence.zip` contains the raw source package and provenance, canonical and generated scenarios, control/disruption/repeat outputs, native simulator metrics and ordered events, full autonomous traces, entity exports, Goal 19 comparison bundle, policy-selection rationale, validation records, this report, and `SHA256SUMS.txt`. The archive has a separate `.sha256` sidecar. Use the included hashes to verify extracted files.

See `vertical.json`, `summary.json`, `analysis.md`, `determinism.json`, and `bundle/index.html` for machine-readable results and the standalone evidence view.
