# Goal 36 — Coordinated Multi-Agent Operations

## Result

Goal 36 adds `COORDINATED_ADAPTIVE` for aircraft surface routing. It shares bounded route-intent and reservation observations among active aircraft, scores candidate routes by local taxi cost plus estimated delay imposed on other agents, and resolves movement contention by longest wait, then higher objective priority, then stable aircraft ID. Route candidate ties retain their deterministic enumeration order. `SCRIPTED` and `ADAPTIVE` remain available with their prior movement ordering and policy behavior.

The system objective is aggregate taxi time and waiting cost. The route scoring estimate combines taxi time, expected reservation wait, a small distance term, and estimated wait imposed on agents whose active route shares an edge. Safety remains outside this objective: closed edges, node and edge reservations, separation checks, and safe-failure behavior remain authoritative.

## Shared state and evidence

The policy receives a bounded list of other active aircraft route uses, including agent ID, edge, destination, estimated release time, current wait age, objective priority, and whether that edge is reserved. At most 32 route-use entries enter any one observation. Candidate evidence includes local score, estimated system cost, affected-agent count, and a representative conflict agent and edge. Decision traces retain the shared observation, candidate list, selected action, rule, and selected conflict where applicable. See [the trace schema example](goal36-evidence/runs/route_choice_interaction/seed-42/coordinated_adaptive/decisions.json).

For resource contention, the coordinator orders movement attempts by descending current wait age, descending objective priority, then ascending stable aircraft ID. This prevents a continuously waiting agent from being overtaken by newer requests while the held reservation releases. It does not promise progress if a resource is never released. Route candidate ties use stable candidate order.

## Experiment

The matrix compares `SCRIPTED`, `ADAPTIVE`, and `COORDINATED_ADAPTIVE` on six synthetic scenario roles and seeds 42, 43, and 44: shared bottleneck, route-choice interaction, route closure, mixed arrival/departure priority conflict, repeated contention for fairness, and a low-benefit baseline. It contains 54 serial runs and 54 parallel replays. Only `surface_operations.aircraft_routing_policy` changes between configured scenario copies.

All 54 serial and parallel runs had matching normalized metrics and SHA-256 digests for `metrics.json`, `events.jsonl`, and `decisions.json`. Repeated same-seed runs are represented by the serial/parallel pair. Summary results and each agent's route objective completion times are in [results.csv](goal36-evidence/results.csv) and [experiment.json](goal36-evidence/experiment.json).

### Measured comparison

Values are means across seeds. Objective completion counts are totals across three runs. Collisions and safe failures were zero for all policies and scenarios.

| Scenario | Policy | Wait (s) | Taxi time (s) | Taxi distance (m) | Departure delay (s) | Throughput (/h) | Completed objectives | Coordination decisions | Contention decisions | Min separation (m) |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Shared bottleneck | SCRIPTED | 105.0 | 735.0 | 672 | 606.7 | 8.85 | 9 | 0 | 0 | 30.0 |
| Shared bottleneck | ADAPTIVE | 105.0 | 735.0 | 672 | 606.7 | 8.85 | 9 | 0 | 0 | 30.0 |
| Shared bottleneck | COORDINATED_ADAPTIVE | 105.0 | 735.0 | 672 | 606.7 | 8.85 | 9 | 27 | 21 | 30.0 |
| Route-choice interaction | SCRIPTED | 273.0 | 941.0 | 780 | 698.7 | 8.51 | 9 | 0 | 0 | 30.0 |
| Route-choice interaction | ADAPTIVE | 294.0 | 1267.0 | 1056 | 797.7 | 7.90 | 9 | 0 | 0 | 26.1 |
| Route-choice interaction | COORDINATED_ADAPTIVE | 294.0 | 1267.0 | 1056 | 797.7 | 7.90 | 9 | 36 | 30 | 26.1 |
| Closure disruption | SCRIPTED | 64.3 | 800.3 | 768 | 625.1 | 8.46 | 9 | 0 | 0 | 38.6 |
| Closure disruption | ADAPTIVE | 64.3 | 800.3 | 768 | 625.1 | 8.46 | 9 | 0 | 0 | 38.6 |
| Closure disruption | COORDINATED_ADAPTIVE | 64.3 | 800.3 | 768 | 625.1 | 8.46 | 9 | 27 | 21 | 38.6 |
| Priority conflict | SCRIPTED | 1137.3 | 1946.0 | 998 | 898.3 | 6.48 | 15 | 0 | 0 | 18.6 |
| Priority conflict | ADAPTIVE | 1137.3 | 1946.0 | 998 | 898.3 | 6.48 | 15 | 0 | 0 | 18.6 |
| Priority conflict | COORDINATED_ADAPTIVE | 1137.3 | 1946.0 | 998 | 898.3 | 6.48 | 15 | 75 | 69 | 18.6 |
| Fairness contention | SCRIPTED | 105.0 | 735.0 | 672 | 606.7 | 8.85 | 9 | 0 | 0 | 30.0 |
| Fairness contention | ADAPTIVE | 105.0 | 735.0 | 672 | 606.7 | 8.85 | 9 | 0 | 0 | 30.0 |
| Fairness contention | COORDINATED_ADAPTIVE | 105.0 | 735.0 | 672 | 606.7 | 8.85 | 9 | 27 | 21 | 30.0 |
| Non-beneficial baseline | SCRIPTED | 553.0 | 1861.0 | 1861 | 1722.3 | 2.51 | 15 | 0 | 0 | 30.0 |
| Non-beneficial baseline | ADAPTIVE | 553.0 | 1861.0 | 1861 | 1722.3 | 2.51 | 15 | 0 | 0 | 30.0 |
| Non-beneficial baseline | COORDINATED_ADAPTIVE | 553.0 | 1861.0 | 1861 | 1722.3 | 2.51 | 15 | 24 | 6 | 30.0 |

The route-choice fixture offered the coordinator a direct route and a shared corridor route. COORDINATED_ADAPTIVE did not improve the system outcome over independent ADAPTIVE: both averaged 294 seconds of wait, 1267 seconds of taxi time, and 797.7 seconds of mean departure delay. SCRIPTED did better in this fixture, averaging 273 seconds of wait and 941 seconds of taxi time. No agent showed a measured local sacrifice that produced a global improvement. The fixture is retained as a failed improvement case rather than tuned after observation.

Across all six scenarios, all objectives completed, with zero objective failures, collisions, or surface safety failures. The lowest measured aircraft separation was 18.6 metres in the mixed priority-conflict scenario for every mode; the safety validators reported no collision or safe-failure event. This is simulator evidence for these synthetic inputs only.

The contention metric counts decisions where another route intent or shared route-use observation was present. `selected_shared_conflict_decisions` separately counts decisions where the chosen candidate itself still shared a conflicted route edge. Full per-agent completion timestamps, reroutes, waits, and trace hashes remain in the evidence bundle.

## Validation

- MSVC Release configure and build with Visual Studio 2026: passed.
- Complete Release CTest suite: 207/207 passed.
- Goal 36 policy, tie-break, wait-cost, and safety regressions: passed.
- Goal 35 adaptive surface and route-choice regressions: included in the passing native suite.
- Goal 36 experiment matrix: 54 serial runs and 54 parallel replays; normalized metrics and all three artifact hashes matched.
- Python syntax compilation and matrix-runner help: passed.
- Dashboard and Scenario Studio suites: 73/73 passed, including the native CLI-backed simulator export case.
- Goal 19 experiment analysis: 10/10 passed.
- Evidence bundle: 13/13 passed.
- Airport ingestion: 13/13 passed.
- Airport scenario generation and run-artifact adapters: 31/31 passed.
- Evidence SHA-256 ledger: 184 files verified against [SHA256SUMS.txt](goal36-evidence/SHA256SUMS.txt).
- `git diff --check`: recorded after final source and evidence updates.

## Limitations and next work

This model reasons about current route intents and reserved edges, not a full time-expanded schedule or uncertainty distribution. The candidate set remains bounded by the existing route enumerator. The fairness rule applies to aircraft surface resource movement and does not cover service-vehicle dispatch, gate assignment, or runway scheduling. Wait and delay weights are engineering estimates rather than airport-calibrated values. The six scenario roles reuse existing synthetic scenarios, so the fairness role is repeated bottleneck traffic rather than a dedicated long-duration starvation stress fixture. The current comparison shows no aggregate gain for the coordinated mode.

Goal 37 should add a dedicated time-expanded intersection reservation fixture and evaluate coordinator forecasts against realized waits before introducing more priority classes. Any global benefit should be demonstrated on held-out synthetic demand patterns, with the same safety and serial/parallel checks.

No results here establish real-airport performance or operational readiness.

## Reproduction

From the repository root:

```powershell
python tools/goal36/run_matrix.py --cli build-goal36-vs18/Release/airside_cli.exe --workers 4
```

The manifest, configured scenario copies, per-run metrics, event streams, decision traces, result table, and SHA-256 ledger are under [docs/goal36-evidence](goal36-evidence/README.md).
