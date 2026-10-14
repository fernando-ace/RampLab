# Goal 41: KAUO airport operations vertical

## Purpose

Goal 41 connects RampLab's existing airport data, simulator, operational policies, dashboard, analysis and evidence paths into one product demonstration. Goal 40 was a useful planner experiment: measured results showed KPI tradeoffs rather than a clearly superior broad operational policy. This goal keeps that research evidence intact and focuses on whether an operator can follow a complete airport operation.

## Vertical architecture

The workflow validates the source package and provenance, rebuilds canonical airport data, and uses the existing scenario generator to create matched control and disruption scenarios. The authoritative C++ simulator runs aircraft, turnaround tasks, ground units, runway movement and safety checks. Its structured decision trace and ordered events feed operator summaries, replay data and Goal 19 KPI comparison. The existing dashboard loads these run bundles and can also start the configured disruption scenario as an interactive live session. The existing evidence-bundle tool packages normalized run data and checks safety fields, event parsing and repeat-run evidence. The Unreal launch path is wired through the live operator view; the editor build could not complete on this workstation because CesiumForUnreal is missing.

## Reproduce the demonstration

Build or provide the Release CLI, then run the workflow into a new or empty output directory:

```powershell
cmake -S . -B build-goal41 -G "Visual Studio 18 2026" -A x64
cmake --build build-goal41 --config Release
python -m tools.goal41.run_vertical --output docs/goal41-reproduction --build-dir build-goal41
```

To load that result and its configured disruption scenario in the local dashboard:

```powershell
python tools/ops_dashboard/server.py --host 127.0.0.1 --port 8766 --real-demo-dir docs/goal41-reproduction --live-cli build-goal41/Release/airside_cli.exe --live-scenario-file docs/goal41-reproduction/generated/disruption/scenario.json
```

The workflow creates control, disruption and repeat runs, analysis, the standalone evidence view, a SHA-256 ledger, the portable ZIP and the Goal 41 report.

## Operational scenario

This vertical runs three illustrative aircraft through arrivals, turnarounds and departures, with nine configured ground units and 33 service tasks. The control and disruption scenarios use the same seed (42), ADAPTIVE aircraft routing policy, source inputs and initial scenario state. At 241 seconds, the disruption case extends Flight KAUO-G31-GA-03 task `G31-03-PUSH` from 180 to 480 seconds. This controlled task-duration event causes the simulator to recompute turnaround readiness.

The C++ RampLab simulator remains authoritative for aircraft, fleet, task, runway and safety outcomes. Agent decisions are exported from its structured decision trace and summarized for operators. Evidence retains native metrics, aircraft rows, fleet assignments, events, full decision traces, batch repeat runs, the static comparison bundle and the completed Dashboard live-session artifacts.

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
| Mean taxi time per aircraft | 396.00 s | 394.00 s | -2.00 s |
| Taxi distance | 5877.35 m | 5877.35 m | 0.00 m |
| Mean taxi distance per aircraft | 1959.12 m | 1959.12 m | 0.00 m |
| Surface wait | 1129 s | 1123 s | -6 s |
| Departure delay | 643.00 s | 937.00 s | 294.00 s |
| Mean departure delay per aircraft | 214.33 s | 312.33 s | 98.00 s |
| Reroutes | 0 | 0 | 0 |
| Task reassignments | 0 | 0 | 0 |
| Minimum aircraft separation | 151.03 m | 948.13 m | 797.10 m |
| Minimum aircraft-ground separation | 13.57 m | 31.55 m | 17.98 m |
| Minimum ground-unit separation | 1.97 m | 1.97 m | 0.00 m |

The simulator reported 0 aircraft-aircraft, 0 aircraft-ground and 0 ground-fleet collisions for the disruption run. Safety failures and failed objectives are separately recorded in `vertical.json`; these simulator results are evidence for the configured scenario only, not an airport safety determination.

## Autonomous operation and replay

- At 241 s, KAUO-G31-GA-03 pushback service was extended by 300 seconds. A controlled service-delay event changed the planned pushback task from 180 to 480 seconds. The simulator recomputed turnaround readiness.

The recorded disruption is handled by RampLab's turnaround scheduler: it applies the configured task-duration event and recomputes readiness. The structured aircraft movement traces are also retained and summarized, but this scenario did not cause a route change or ground-unit reassignment. The measured operation consequence is an increase of 294 seconds in aggregate departure delay; the configured pushback task itself increases by exactly 300 seconds. The small aggregate difference reflects the simulator's schedule and event timing, and the report does not attribute a 300-second increase to every departure.

The full event streams are retained in order. The report does not reconstruct unrecorded aircraft or vehicle motion. Repeat control and repeat disruption runs were checked by the Goal 19 determinism utility; statuses are recorded in `vertical.json` and `determinism.json`.

## Validation

- Data ingestion: PASS.
- Scenario generation: PASS.
- Matched initial state: PASS.
- Scenario completion and exported safety fields: PASS.
- Repeat-run determinism: PASS.
- Release CTest: 218/218 passed.
- Focused Python validation: 144 tests passed; 6 were skipped by existing test conditions.
- Dashboard browser review: No rendered browser review is recorded for this output directory.
- Scenario Studio browser review: No rendered Scenario Studio review is recorded for this output directory.
- Unreal Editor build was attempted with UE 5.8 and could not proceed because the enabled CesiumForUnreal plugin is not installed on this workstation.

## Product assessment

RampLab now demonstrates a coherent airport-operations autonomy vertical for this configured KAUO simulation: an operator can identify the operation, observe aircraft and mobile units, review the controlled service disruption and scheduler response, compare completion and KPI results, and inspect replay and evidence. This does not establish production readiness or real-airport performance.

The largest remaining gaps are calibrated, authoritative taxiway/apron geometry and live airport feeds; a verified spatial correlation between Unreal state and dashboard events; richer measured vehicle-position and queue metrics; and validation with operationally representative schedules, equipment and demand. The controlled pushback delay tests integration behavior only, and the missing Cesium plugin prevented a workstation Unreal visualization check.

## Portable evidence

`goal41-portable-evidence.zip` contains the raw source package and provenance, canonical and generated scenarios, control/disruption/repeat outputs, the interactive completed live session, native simulator metrics and ordered events, full autonomous traces, entity exports, Goal 19 comparison bundle, policy-selection rationale, `ui-validation.json`, validation records, this report, and `SHA256SUMS.txt`. The archive has a separate `.sha256` sidecar. Use the included hashes to verify extracted files.

See `vertical.json`, `summary.json`, `analysis.md`, `determinism.json`, and `bundle/index.html` for machine-readable results and the standalone evidence view.
