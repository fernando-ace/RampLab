# Goal 40: Cost-aware counterfactual operational planning

## Scope and evidence boundary

Goal 40 extends Goal 39's `COUNTERFACTUAL` policy with a separate `COST_AWARE_COUNTERFACTUAL` mode. The old policy remains available and retains its Goal 39 lexicographic ranking. Both modes use the same bounded private rollout, current revealed information, candidate generation, live feasibility checks, and simulator safety authority.

The experiment uses ten deterministic synthetic scenarios and seeds 42, 43, and 44. These results describe RampLab simulator runs only. They do not establish real-airport or KAUO operational performance.

## 1. The problem exposed by Goal 39

Goal 39 ranked successful rollouts by downstream waiting before taxi time and distance. That made a detour attractive whenever it removed predicted wait, even when the route added much more time and travel. In its 30-run `COUNTERFACTUAL` matrix, mean waiting fell to 292.57 seconds per run from 408.50 seconds for `ROLLING_HORIZON_COORDINATED`, while taxi time rose to 1,279.90 seconds and distance to 1,158.20 metres.

Waiting alone is not a sufficient measure of operational cost. A route that saves a few seconds by adding a long taxi leg can delay objective completion, use more pavement, and create more replanning. Goal 40 prices those terms directly and requires a material improvement before replacing a baseline.

## 2. Cost model and normalization

The public `OperationalCostConfig` holds dimensionless weights, metric scales, thresholds, fairness settings, and the replan cooldown. Defaults are recorded in every cost-aware decision trace and in the experiment configuration:

| Term | Weight | Normalization limit |
|---|---:|---:|
| Predicted downstream wait | 0.40 | 600 seconds |
| Predicted completion time | 0.30 | 600 seconds |
| Predicted route distance | 0.18 | 2,000 metres |
| Predicted contention overlap | 0.08 | 600 seconds |
| Changing from the current route | 0.04 | Binary penalty |

For each time or distance term, the measured value is clipped to its configured limit and divided by that limit before applying its weight. This keeps seconds and metres comparable. The selected candidate minimizes the sum of those dimensionless terms. A route change is charged only when an alternate replaces an existing route; requesting the first route has no churn penalty.

The bounded rollout still has the Goal 39 limits: 600 seconds, eight route edges, and 256 private model events per candidate. Route candidates that do not reach the objective within the same bounded horizon cannot defeat a candidate with a better completion/block outcome. This completion ordering is an eligibility constraint, separate from safety and scalar cost.

The default weight values sum to one. They can be changed as a validated `OperationalCostConfig`; the current simulator scenario schema does not expose per-scenario weight overrides.

## 3. Material improvement, bounded regret, and stable ties

The baseline is the feasible current route when its bounded rollout has the best completion outcome. When no such current route exists, the baseline is the lowest local candidate score among candidates with the best completion outcome.

An alternative must improve the baseline score by at least `0.01`. It must also satisfy the bounded-regret rule:

```text
additional non-wait cost <= 0.85 * reduction in weighted waiting cost
```

This prevents a small waiting gain from buying a much larger taxi-time, distance, or route-change cost. The ratios are dimensionless and come from the same normalized cost terms. An exact cost tie retains the earlier candidate in stable route enumeration order. If an alternate is blocked by the existing ten-second replan cooldown, the current route remains selected.

## 4. Fairness and starvation protection

Waiting aircraft receive an increasing wait term. Its multiplier is:

```text
1 + 0.50 * min(wait_age_seconds / 120, 1)
  + 0.25 * clamp(objective_priority / 10, 0, 1)
```

The simulator continues to arbitrate movement in wait-age, objective-priority, then stable aircraft-ID order. The cost model does not replace that ordering. All scenario-matrix aircraft currently have objective priority zero, so the matrix does not measure priority differentiation. The targeted fairness fixture reuses real rollout predictions, then re-evaluates them with a 120-second wait age and priority 10 to prove those signals can change the policy decision. This is a controlled fairness sensitivity case; it does not prove fleet-wide freedom from starvation over long schedules.

## 5. Structured decision traces

For each cost-aware candidate, `decisions.json` records its route/action kind, bounded-rollout wait, taxi and completion time, distance, contention count and delay, objective-completion eligibility, and whether it is the baseline. Its `operational_cost` object records the normalized terms, route-change penalty, fairness multiplier/contribution, total cost, improvement against baseline, materiality result, and bounded-regret result. The decision trace includes the configured weights and limits, objective priority, wait age, selected candidate, tie rule, and safety constraints.

The cost trace is structured JSON, not additional normal console logging. Invalid routes remain ineligible regardless of score. Live route feasibility, reservations, closures, separation, collision prevention, and other simulator safety checks still decide whether movement can occur.

## 6. Targeted fixtures

The fixtures use actual Goal 40 planner decisions and bounded rollout predictions from the full simulation tests. Fixture inputs and complete traces are in `targeted_fixtures.json` and `representative_decision_traces.json` inside the evidence package.

| Fixture | Actual rollout comparison | Goal 40 result |
|---|---|---|
| Wasteful detour: `closure_during_execution`, seed 42, aircraft-2 at 848 s | Current route: 33 s wait, 160 s taxi, 160 m. Alternate: 0 s wait, 256 s taxi, 256 m. | Keeps current route. The 96 s and 96 m increase has total cost 0.191 versus 0.136 and fails both the 0.01 material threshold and regret gate. |
| Worthwhile detour: `individual_route_choice`, seed 42, aircraft-3 at 578 s | Current route: 122 s wait, 200 s taxi, 200 m. Alternate: 0 s wait, 280 s taxi, 280 m. | Selects the alternate. Cost falls from 0.237 to 0.205; improvement 0.032 passes materiality and bounded regret. |
| Wait-age and priority: `long_corridor_congestion`, seed 42, aircraft-1 at 801 s | Current route: 57 s wait, 250 s taxi, 250 m. Alternate: 0 s wait, 322 s taxi, 322 m. | At the recorded 30 s wait age and priority 0, retains the route. With the same rollout and route predictions at wait age 120 and priority 10, the multiplier rises from 1.125 to 1.75; the alternate's score improvement becomes 0.01252 and its 0.05398 non-wait regret fits the 0.056525 allowance, so the planner changes to it. |

The first two fixtures prove that the policy can reject and accept detours using actual planner rollouts. The fairness fixture proves the cost-aware decision responds to aging and priority on an actual rollout state; it does not claim a long-running schedule has no starvation.

## 7. Policy comparison experiment

The ten-scenario Goal 39 matrix was preserved. Each source scenario was copied with only `surface_operations.aircraft_routing_policy` changed. The experiment compared six modes over seeds 42, 43, and 44: `SCRIPTED`, `ADAPTIVE`, `COORDINATED_ADAPTIVE`, `ROLLING_HORIZON_COORDINATED`, Goal 39 `COUNTERFACTUAL`, and Goal 40 `COST_AWARE_COUNTERFACTUAL`.

There were 180 serial runs and 180 parallel replays. The table reports the equal-weight mean of the ten scenario means across three seeds. Objective completions, failures, collisions, and safety failures are sums over the 30 runs for each policy. Runtime is mean complete CLI time per run, including startup, simulation, and output I/O.

| Policy | Wait s/run | Taxi s/run | Distance m/run | Objectives complete | Failures | Collisions | Safety failures | Replans | Decisions | CLI s/run |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| SCRIPTED | 425.20 | 1,199.63 | 963.40 | 108/108 | 0 | 0 | 0 | 9 | 309 | 0.3291 |
| ADAPTIVE | 408.50 | 1,221.83 | 1,002.60 | 108/108 | 0 | 0 | 0 | 39 | 483 | 0.3288 |
| COORDINATED_ADAPTIVE | 408.50 | 1,221.23 | 1,002.60 | 108/108 | 0 | 0 | 0 | 30 | 489 | 0.3383 |
| ROLLING_HORIZON_COORDINATED | 408.50 | 1,221.23 | 1,002.60 | 108/108 | 0 | 0 | 0 | 9 | 489 | 0.3389 |
| COUNTERFACTUAL (Goal 39) | 292.57 | 1,279.90 | 1,158.20 | 108/108 | 0 | 0 | 0 | 6 | 387 | 0.3364 |
| COST_AWARE_COUNTERFACTUAL (Goal 40) | 406.40 | 1,191.63 | 975.00 | 108/108 | 0 | 0 | 0 | 15 | 492 | 0.3382 |

Goal 40 reduced taxi time by 88.27 seconds per run (6.9%) and distance by 183.20 metres (15.8%) relative to Goal 39. Waiting increased by 113.83 seconds per run (38.9%). Relative to the rolling-horizon baseline, Goal 40 reduced wait by only 2.10 seconds (0.5%), taxi time by 29.60 seconds (2.4%), and distance by 27.60 metres (2.8%). It therefore gave up almost all of Goal 39's waiting reduction while lowering the travel penalties. It does not dominate Goal 39: Goal 39 has lower wait, and Goal 40 has lower taxi time and distance.

The Goal 40 policy evaluated 1,047 candidates and 3,108 private rollout events, compared with 699 candidates and 2,097 events for Goal 39. It recorded 492 planner decisions and 15 replans, versus 387 decisions and six replans for Goal 39. The keyed `(aircraft_id, simulation_time)` comparison found 243 of Goal 39's 387 decision records changed or unmatched; shifted timestamps can account for some unmatched records, so this is not a causal per-decision outcome measure.

Mean full-process time was 0.3382 seconds for Goal 40 and 0.3364 seconds for Goal 39, a 1.9 ms difference. This is an overhead proxy rather than isolated planner profiling. The deterministic candidate and event counts are the reproducible planner-work measures.

## 8. Safety and determinism

Across the complete six-policy matrix there were zero collisions, zero surface safety failures, and zero objective failures. Each policy completed 108 of 108 objectives. The minimum aircraft separation over the matrix was 18.601633 m. The targeted simulator fixtures also completed without collisions, safe failures, or objective failures.

All 180 serial runs matched their corresponding parallel replay in normalized metrics and SHA-256 digests for normalized output, metrics, events, and decisions. No random tie-breaking, wall-clock-dependent decision logic, or scheduling-dependent candidate iteration was added. Full CTest passed 224/224 tests; the requested Python analysis and evidence suites passed 30/30 tests.

The Unreal-linked C++ core libraries also built successfully with the pinned MSVC 14.44 toolset. The Unreal editor target could not be validated in this worktree because its required `CesiumForUnreal` plugin is absent; UnrealBuildTool stopped during project setup before compiling project modules.

## 9. Limitations and conclusion

- The route rollout is a bounded edge-event model, not a second full airport lifecycle simulation. It does not model future unrevealed events, uncertainty, or other agents' future replanning.
- The matrix contains ten synthetic scenarios and three fixed seeds. It cannot establish real-airport performance or statistical significance.
- Objective priorities in the experiment scenarios are all zero. The high-priority fairness sensitivity case exercises the planner using one real simulator rollout, but a scenario-level aircraft priority field is not present in this experiment input schema.
- The contention term uses predicted overlap from currently revealed traffic intents. It is not an airport-wide estimate of all future congestion.
- Goal 40's aggregate wait is close to the rolling-horizon baseline, so it fails the primary test of retaining a meaningful portion of Goal 39's waiting reduction. Its taxi-time and distance results are better, but the data do not support further work on this aircraft planner as the next step.

No specific bounded planner follow-up is supported by this evidence. The next RampLab direction should be the higher-level operational objective, planning/dispatch, human interaction, scenario execution, and end-to-end demonstration work already identified for AutoVerse-style capability.

**STOP PLANNER ITERATION**

## Reproduction and evidence

From the repository root, using the Visual Studio 2026 x64 generator:

```powershell
cmake -S . -B build-goal40 -G "Visual Studio 18 2026" -A x64 -DBUILD_TESTING=ON
cmake --build build-goal40 --config Release --target airside_tests airside_cli --parallel 6
ctest --test-dir build-goal40 -C Release --output-on-failure
python -m unittest tools.evidence_bundle.test_bundle tools.goal38.test_goal38 tools.goal39.test_goal39 tools.goal40.test_goal40 -v
python tools/goal40/run_matrix.py --cli build-goal40/Release/airside_cli.exe --workers 4 --output build-goal40/goal40-evidence-reproduction
python tools/goal40/build_bundle.py --evidence-root build-goal40/goal40-evidence-reproduction --output build-goal40/goal40-evidence-reproduction/portable-bundle
```

Goal 19's legacy analysis test must be run from its own directory because its test module imports `analyze` as a top-level module:

```powershell
Push-Location tools/experiment_analysis
python -m unittest test_analyze -v
Pop-Location
```

The verified bundle is `docs/goal40-evidence/portable-bundle/`. It contains `experiment.json`, `summary.json`, `results.csv`, all serial raw run outputs, exact scenario copies, the three targeted traces, the report, a SHA-256 ledger, and a ZIP archive. The bundle contains 798 checksummed files; the ZIP has 799 entries including its ledger.
