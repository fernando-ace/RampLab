# Goal 37: Coordinated decision quality

## Objective and scope

Goal 37 adds a system-level cost to the existing deterministic operational-agent decision process. The coordinator compares route candidates using their local route cost plus estimated delay imposed on other aircraft sharing planned route edges. It does not replace surface separation, collision checks, runway arbitration, or other authoritative simulator safety controls.

All four new scenarios and the controlled comparison are synthetic fixtures. They do not represent measured KAUO operations or calibrated airport geometry.

## Implementation

At each decision, the simulator provides the agent with planned route intents and forecasts routes for ready-for-pushback aircraft that do not yet have a plan. Forecasted intents are used only by `COORDINATED_ADAPTIVE`, so existing `SCRIPTED` and `ADAPTIVE` behavior remains unchanged. Shared route-use edges are collected in stable aircraft order and capped at 32 edges.

For a candidate, each other aircraft with overlapping remaining route edges contributes its maximum shared-edge traversal time, adjusted for its accumulated wait age and configured objective priority. The implementation uses the bounded adjustment:

`traversal_seconds * (1 + min(wait_age_seconds, 120) / 600 + clamp(priority, 0, 10) / 20)`

Those contributions form `estimated_wait_imposed_seconds`, which is added to the candidate's local cost to form `estimated_system_cost_seconds`. Existing deterministic tie-breaking and safety evaluation remain in place. Decision traces include candidate costs, affected aircraft, selected status, and rejection explanations. The policy explanation identifies when a lower-local-cost option was rejected because its estimated system cost was higher.

## Evaluation design

`tools/goal37/run_matrix.py` evaluates 10 scenarios, three policies (`SCRIPTED`, `ADAPTIVE`, `COORDINATED_ADAPTIVE`), and seeds 42, 43, and 44: 90 serial runs plus 90 parallel replays. It records aggregate surface wait, taxi time and distance, departure delay and throughput, objective completion, collision and safety outcomes, minimum separation, reroutes, and coordination decisions. Each serial result is compared against its parallel replay for deterministic equivalence.

Scenario set:

- Six established Goal 36 cases: shared bottleneck, route-choice interaction, closure disruption, priority conflict, fairness contention, and non-beneficial coordination.
- Four new synthetic cases: shared runway-entry queue, long shared corridor, early closure merge, and coordinated route choice.

## Results

The matrix reports 90 serial runs and 90 parallel replays, with determinism passing. Across the matrix there were no collisions, safety failures, or objective failures; minimum aircraft separation was 18.6 m. The coordinated policy made 333 coordination decisions across its 30 scenario-seed runs. Those decisions did not change aggregate outcomes in nine of the ten scenarios.

The controlled synthetic `coordinated_route_choice` case is the one measurable improvement. Means across three seeds:

| Policy | Surface wait | Taxi time | Taxi distance | Mean departure delay |
|---|---:|---:|---:|---:|
| SCRIPTED | 213 s | 396 s | 780 m | 548.67 s |
| ADAPTIVE | 213 s | 396 s | 780 m | 548.67 s |
| COORDINATED_ADAPTIVE | 213 s | 390 s | 780 m | 548.67 s |

In this fixture, coordination reduces taxi time by 6 seconds (about 1.5%) without changing wait, distance, or departure delay. At aircraft-3's first route decision, the chosen direct route had system cost 206.6. A lower-local-cost shared route had system cost 240 and an estimated 75 seconds imposed on one other aircraft. The structured trace and explanation are in `docs/goal37-evidence/runs/coordinated_route_choice/seed-42/coordinated_adaptive/decisions.json`.

The improvement is narrow and scenario-specific. In the existing `route_choice_interaction` and new `long_shared_corridor`, both adaptive policies choose the same route and the existing scripted policy has lower aggregate taxi time. The coordinator did not improve aggregate metrics in the other tested cases.

## Limitations

- The model uses overlapping planned/forecast route edges and edge traversal times as a delay proxy; it is not a time-expanded traffic scheduler.
- Forecast routes for unplanned aircraft use the deterministic shortest-route calculation.
- The shared intent budget is 32 edges. Wait-age and priority adjustments are bounded, hand-tuned weights rather than empirically calibrated parameters.
- The evidence covers deterministic synthetic scenarios and three fixed seeds; it does not establish performance across real airports, uncertain traffic, or operational distributions.
- The measured benefit is six seconds in one fixture. No broad performance claim is supported by this result.

## Reproduction and evidence

Run from the repository root after configuring and building the Release targets:

```powershell
cmake -S . -B build-goal37 -G "Visual Studio 18 2026" -A x64 -DBUILD_TESTING=ON
cmake --build build-goal37 --config Release --target airside_tests airside_cli --parallel 6
ctest --test-dir build-goal37 -C Release --output-on-failure
python tools/goal37/run_matrix.py --cli build-goal37/Release/airside_cli.exe --output docs/goal37-evidence
python tools/goal37/build_bundle.py --output docs/goal37-evidence/portable-bundle-final
python -m py_compile tools\goal37\run_matrix.py tools\goal37\build_bundle.py
git diff --check
```

The final portable bundle's `validation.json` reports `PASS` for validation and determinism. `docs/goal37-evidence/SHA256SUMS.txt` is the file-level integrity ledger. The full matrix, configured scenario copies, per-run metrics/events/decision traces, tabular results, experiment metadata, and portable bundle are in `docs/goal37-evidence/`.

## Commits

- Implementation and scenarios: `b3e9676` (`Add coordinated decision cost evaluation`).
- Evidence and report are committed separately so the implementation commit remains easy to inspect.
