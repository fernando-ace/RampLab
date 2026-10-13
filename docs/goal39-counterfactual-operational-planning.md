# Goal 39: Counterfactual operational planning

## Scope and evidence boundary

Goal 39 adds the `COUNTERFACTUAL` aircraft routing policy. Goal 35 tested individual adaptive route choice; Goal 36 coordinated decisions; Goal 37 externality aware coordination; Goal 38 anticipatory rolling horizon scoring; Goal 39 replaces increasingly elaborate handcrafted predictions with bounded evaluation of simulated route and traffic consequences.

The work is based on Goal 38 commit `a6cc81f5971605d6b62e56a92688e081bb094bd2` in branch `goal39-counterfactual-operational-planning`. It does not modify the primary checkout. All experiment scenarios are synthetic simulator fixtures; none establish real airport operational performance.

## Planner and safety boundary

At an eligible route decision, RampLab first generates the existing deterministic set of currently feasible route candidates (up to six alternatives), plus a feasible current-route continuation and a hold action when the authoritative simulator reports a reservation block. The counterfactual evaluator copies the currently revealed graph and the current observation's reservations and published/observed traffic intents. It evaluates each candidate independently in this private snapshot, then the normal policy selects one route. Only that route is passed back to the live simulator. A new observation is evaluated at the next eligible decision point.

Each candidate rollout uses a deterministic edge event model with a 600 second horizon, at most eight route edges, and at most 256 edge-entry, edge-release, and traffic-conflict events. The route set is capped by the existing generator; because a current route and a hold candidate may also be present, the implementation permits at most eight actions. Alternate-route reconsideration has a ten second cooldown. These caps bound a decision to at most 2,048 private model events. A rollout that cannot reach its objective inside the horizon is marked blocked in that rollout; this is a planning outcome, not a live objective failure.

Candidates are compared lexicographically: (1) more completed objectives in the bounded rollout, (2) fewer blocked objectives, (3) less downstream waiting across the chosen route and affected observed traffic, (4) less taxi time, (5) less route distance, and (6) stable candidate enumeration order. No weighted safety score is used. Invalidated routes bypass preferences and use the existing feasible-route path.

Live graph availability, current edge and node reservations, current published routes, current observed aircraft state, configured speeds, and already revealed closures are legitimate inputs. Scenario event queues, unrevealed future closures or failures, and future random outcomes are not read by the rollout. Ready traffic without a plan uses a route derived from the currently available graph. This is a compact edge-event world model: it does not run another complete copy of the airport lifecycle or let other agents replan during the lookahead.

Route feasibility, edge and node reservation arbitration, separation, aircraft and ground-vehicle clearance, collision prevention, controlled stops, and disruption handling remain authoritative in the live simulator. A favorable rollout cannot authorize an unsafe movement.

## Decision evidence and determinism

Decision JSON records a stable state identifier, trigger, current observation and traffic intents, the selected candidate, each candidate's rollout completion/block status, wait, taxi time, distance and simulated event count, rejection explanation, horizon, tie rule, safety boundary, and whether the chosen action differs from a same-observation `ROLLING_HORIZON_COORDINATED` decision. Reproducible work counts are included in traces. Wall-clock process runtime is measured separately because wall time would make otherwise identical decision traces differ.

Candidate ordering follows the route generator's stable enumeration and current observation's ordered traffic list. Rollouts only read copied graph/traffic state and modify candidate result records. Identical initial state, seed, configuration and candidate ordering produce the same decisions and trace. The experiment harness compares serial runs with deterministic parallel replays by normalized KPI values and SHA-256 hashes of metrics, ordered events, and decisions.

The controlled synthetic `surface_traffic.yaml` bottleneck is exercised by `SurfaceTrafficTest.SyntheticBottleneckRolloutCanTradeTaxiDistanceForLessDownstreamWait`: a live observation has a 160 m current route with a 33 second observed hold and a 256 m feasible alternate without that wait. The actual private rollout selects the longer alternative because its lexicographic downstream wait is lower. `OperationalAgentPolicyTests.SyntheticDownstreamOutcomeCanOverrideLocallyCheapestRoute` separately checks that a locally attractive candidate loses when its rollout blocks completion. These fixtures verify planner behavior and are not airport-performance evidence.

## Evaluation

The Goal 38 representative ten-scenario matrix is retained without selectively removing cases. Goal 39 compares `SCRIPTED`, `ADAPTIVE`, `COORDINATED_ADAPTIVE`, Goal 38's `ROLLING_HORIZON_COORDINATED` policy (the requested ROLLING_HORIZON baseline), and `COUNTERFACTUAL` across seeds 42, 43, and 44, with serial and parallel replays. Policy scenario copies differ only in `surface_operations.aircraft_routing_policy`.

Outcomes that differ from `ROLLING_HORIZON_COORDINATED` are classified per matched scenario and seed using a documented lexicographic run comparison: objective completions, objective failures, waiting, taxi time, then taxi distance. This is a descriptive paired comparison, not causal attribution to a single changed decision. Planner work is reported as rollout/candidate/event counts. Complete CLI process time by policy is reported as an overhead proxy; it includes ordinary simulation and I/O time, so it is not isolated CPU profiling.

### Results

The full matrix contains 150 serial runs and 150 parallel replays. All 150 normalized results and SHA-256 artifact checks matched. Every policy completed 108/108 objectives across its 30 runs, with zero objective failures, collisions, or surface safety failures. Minimum aircraft separation was 18.602 m for the full matrix.

The mean across the ten equally weighted scenario means was:

| Policy | Taxi seconds/run | Wait seconds/run | Taxi distance metres/run |
|---|---:|---:|---:|
| SCRIPTED | 1199.6 | 425.2 | 963.4 |
| ADAPTIVE | 1221.8 | 408.5 | 1002.6 |
| COORDINATED_ADAPTIVE | 1221.2 | 408.5 | 1002.6 |
| ROLLING_HORIZON_COORDINATED | 1221.2 | 408.5 | 1002.6 |
| COUNTERFACTUAL | 1279.9 | 292.6 | 1158.2 |

COUNTERFACTUAL completed the same number of objectives and reduced mean waiting by 115.9 seconds per run (28.4%) relative to ROLLING_HORIZON_COORDINATED. It used 58.7 more taxi seconds (4.8%) and 155.6 more metres (15.5%). That pattern follows the documented lexicographic objective, which prefers less waiting before taxi time and distance. It is a tradeoff, not a broad fleet-performance win.

Across 387 COUNTERFACTUAL decisions, 63 (16.3%) selected a different action or route than ROLLING_HORIZON_COORDINATED would select from the same observation. At least one decision differed in 27 of the 30 matched scenario/seed cases. Among those 27 cases, the final run-level lexicographic comparison classified 21 as improved, three as worsened, and three as unchanged; the remaining three cases had no changed action. The comparison credits the full run outcome to the policy pair and cannot attribute an outcome to one isolated decision.

The changed-action outcomes were spread across seven scenario families, where lower waiting often came with longer routes. The `little_coordination_benefit` baseline worsened in all three seeds: mean taxi time and distance each rose by 96, while waiting was unchanged. `shared_route_choice` had changed actions but no final KPI difference; `early_closure_merge` had no changed actions. COUNTERFACTUAL can therefore make performance worse and did not improve every case.

A representative `shared_bottleneck`, seed 42 decision at 848 seconds shows the reason. Aircraft 2's current route was 160 m and predicted 33 seconds of waiting behind the current reservation. An existing 256 m alternative had no predicted wait. COUNTERFACTUAL selected the longer route because waiting precedes taxi time in its objective. This is inspectable decision evidence, not proof that the detour improves real operations. The complete trace, including all candidate predictions and safety notes, is in `representative_decision_traces.json`.

COUNTERFACTUAL evaluated 699 candidates across the 30 runs and processed 2,097 private rollout events. Its mean whole-CLI runtime was 0.3291 seconds per run versus 0.3299 seconds for ROLLING_HORIZON_COORDINATED. The 30 matched runtime differences ranged from -25.6 to +26.4 ms, with a median of +2.6 ms. These measurements include startup, simulation, and file I/O and do not isolate planner CPU time; they show no measurable positive wall-clock overhead at this scale. The deterministic rollout/event counts are the reproducible work measure.

Scenario-by-scenario metrics, per-aircraft taxi times, all raw metrics/events/decision traces, exact input copies, representative traces, replay checks, and checksums are in [the portable evidence bundle](goal39-evidence/portable-bundle/README.md), its `experiment.json`, and `results.csv`.

## Limitations

- The rollout is a bounded surface edge-event model, not a full cloned simulation of gates, turnaround services, runway operations, other agents' future replanning, or future events.
- Current intent timing is estimated from current routes, reservations, and configured speed; the evaluator does not model uncertainty.
- Completion within 600 seconds and eight edges is a bounded-horizon indicator; a route outside that bound is marked blocked for candidate ranking even when the live simulator could eventually finish it.
- The event budget can truncate unusually dense candidate/traffic interactions. Its count is recorded for review.
- Runtime numbers include the complete CLI process and are machine/load dependent; deterministic work counters are the reproducible cost measure.
- Among equal-completion outcomes, the ranking intentionally puts downstream waiting ahead of taxi duration and distance. The aggregate means show that this can reward longer travel; readers should examine all KPIs instead of treating the lexicographic label `improved` as improvement on every measure.
- The matrix contains fixed synthetic scenarios and three deterministic seeds. It cannot establish benefit under real-airport traffic distributions, and small paired differences are not statistically significant.
- RampLab remains a simulator and research prototype. Synthetic or calibrated scenarios do not establish real airport operational performance.

## Reproduction

From a Visual Studio 2026 x64 environment at the repository root:

```powershell
cmake -S . -B build-goal39 -G "Visual Studio 18 2026" -A x64 -DBUILD_TESTING=ON
cmake --build build-goal39 --config Release --target airside_tests airside_cli --parallel 6
ctest --test-dir build-goal39 -C Release --output-on-failure
python tools/goal39/run_matrix.py --cli build-goal39/Release/airside_cli.exe --output docs/goal39-evidence
python tools/goal39/build_bundle.py --evidence-root docs/goal39-evidence --output docs/goal39-evidence/portable-bundle
python -m unittest tools.experiment_analysis.test_analyze tools.evidence_bundle.test_bundle tools.goal38.test_goal38 tools.goal39.test_goal39 -v
```
