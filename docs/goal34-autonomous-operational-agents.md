# Goal 34: Autonomous Operational Agents

## Scope and safety boundary

Goal 34 adds deterministic aircraft surface-routing policies around the existing RampLab simulation. It does not replace the simulator's reservation, collision, or movement rules. A policy proposes a route decision from a structured observation; the simulation validates and executes that decision through its established path. No language model or nondeterministic action selection is involved.

The implementation is intentionally limited to aircraft routing, the mobile entity available on the Goal 33 baseline. The interfaces are general enough to add other agent types later, but no Goal 31 GSE/service behavior is copied here.

## Agent model

`airside::agents::OperationalAgent` carries a stable ID, type, objective, current status, selected policy, policy memory, and decision history. Current aircraft objectives express a destination or operational completion goal. The routing observation contains current/destination nodes, current route, closure and blockage information, and simulation time. It is built at decision points rather than exposing unrestricted simulation state to the policy.

Policies return a typed decision containing an action, reason code, explanation data, route candidate, objective result, and policy identity. `SCRIPTED` preserves the legacy shortest-route selection behavior. `ADAPTIVE` uses the same safe route planner while applying deterministic expected-wait and reroute-threshold/cooldown behavior. A closure invalidating a route triggers immediate replanning; congestion is reconsidered only after sustained blockage. Failed route objectives are reported as failures rather than successful completion.

## Decision and execution flow

1. The simulator creates an observation for an aircraft decision point.
2. The configured policy proposes a route/action and structured reason.
3. Existing route availability and reservation logic validates whether the aircraft can proceed.
4. Existing movement, occupancy, and collision protections execute the operation.
5. Decision events and resulting observations are available for inspection.

Operator closure updates invalidate affected plans and record an operator-triggered decision. Policies cannot mutate reservations or bypass safety checks.

## Event trace

The `AgentDecision` simulation event and JSONL event output record the event/agent IDs, agent type, policy, objective, action, reason, observation summary, decision result, selected route, and previous route when a plan changes. The Scenario Studio Agents view reads completed native `events.jsonl` output and displays the policy, objective, latest reason, route transition, and decision history. It does not synthesize trace rows.

The Goal 32 parameter registry exposes `agent_policy.aircraft_routing` as a categorical scenario parameter. The canonical Scenario Studio scenario stores the same choice and Goal 24B generation writes it into the native `surface_operations.aircraft_routing_policy` setting. The existing experiment runner can therefore vary policies without an engine-specific branch.

## Determinism and validation

Given the same scenario, observations, and policy state, route decisions use deterministic ordering and stable reason codes. The new integration regression exercises an unscripted response to a scheduled closure, asserts safe completion, and compares repeated traces. Operator closure behavior has a separate regression. The full CTest and Python/browser checks are the release gate; these results are recorded after final validation.

Final validation on this Goal 34 worktree: Release CTest passed 193/193; the Scenario Studio/dashboard Python suite passed 71 tests with one skip; the live-control Python integration passed 5/5; JavaScript syntax, edited Python compilation, evidence SHA-256 verification, and `git diff --check` passed. The required Goal 24A/24B/19/evidence Python aggregate ran 66 tests. The normal run had one environment-specific mismatch: `test_naive_timezone_ambiguous_time_fails_without_database` expects IANA timezone data to be unavailable, but it is installed on this Windows host. I reran the same 66 tests with only that test's `ZoneInfo` lookup isolated to simulate the missing-database condition it names; all 66 passed. No product or test source was changed for that environment adjustment.

## KAUO prototype evidence and limits

The demonstration uses the KAUO Goal 27 geometry with an assumed two-aircraft schedule and a route closure. That schedule is a simulation fixture and the geometry is approximate; this is not a surveyed airport digital twin or evidence about observed flight operations. The closure invalidates a committed departure route and the agent selects a north-side detour from the current network. Both policy modes complete the fixture without surface safety failures or collisions. This case demonstrates replanning, not an adaptive performance advantage; the two modes can produce the same result when congestion does not distinguish them.

The Goal 32 experiment manager also ran a two-configuration `SCRIPTED`/`ADAPTIVE` sweep on this scenario with seed 42 and hard zero-collision/zero-surface-failure constraints. Both native runs validated and were feasible. Mean departure delay was 21.7167 minutes for each, so this experiment found no delay advantage for either policy. Both remain on the one-objective Pareto frontier.

For parity, the same policy matrix, seed, scenario, and disabled-cache setting was run once with one worker and once with two. Both experiments completed all 2 runs without failures. Per-policy normalized result digests and metrics matched exactly. This establishes Goal 32 serial/parallel parity for this tested Goal 34 scenario; it does not establish parity across every Goal 32 configuration.

Policy overhead was measured with 10 repeated real native CLI runs per policy, alternating policy order and excluding one warm-up per mode. On this Windows host, median process wall time was 0.5210 s for SCRIPTED and 0.5165 s for ADAPTIVE; population standard deviations were 0.0308 s and 0.0205 s. The adaptive/scripted median ratio was 0.9915. This small fixture and host-specific timing do not support a general speedup claim. Full samples and executable/scenario hashes are in [goal34-agent-overhead-benchmark.json](goal34-agent-overhead-benchmark.json).

Browser screenshots captured from the real Scenario Studio run and experiment results:

- [Agents overview with a real selected aircraft, policy, and route map](screenshots/goal34-agents-adaptive-replan.png)
- [Scenario Studio aircraft routing policy selection](screenshots/goal34-policy-selection.png)
- [Native decision trace including the closure replan at simulation second 1532](screenshots/goal34-autonomous-replan-event.png)
- [Completed Goal 32 policy experiment](screenshots/goal34-policy-experiment.png)
- [Experiment result table and policy sensitivity/Pareto panels](screenshots/goal34-policy-results.png)
- [Goal 33 Plan scenario with adaptive aircraft agents](screenshots/goal34-goal33-plan-agents.png)
- [Operator intervention followed by a structured replanning decision](screenshots/goal34-operator-replan.png)
- [Goal 33 live route update followed by an ADAPTIVE agent response](screenshots/goal34-goal33-live-update-replan.png)

The copied native event streams, generated scenarios, simulator metrics, Goal 19 outputs, hashes, and Goal 32 experiment summaries are in [goal34-evidence/README.md](goal34-evidence/README.md). The operator live-session evidence uses the legacy `SCRIPTED` policy: an operator closure at second 1504 invalidated the departure plan, and the agent selected a replacement at second 1532. It then completed with zero objective failures, zero surface safety failures, and zero aircraft collisions. This verifies operator intervention response while retaining the policy label in the trace; it is not presented as an ADAPTIVE-policy run.

For Goal 33 integration, an existing imported KAUO Plan scenario was copied into a new run with only `agent_policy.aircraft_routing=ADAPTIVE` added. Its persisted provenance identifies source file `kauo-tomorrow-schedule.csv`, three source records, and operational date 2026-10-12. The native run completed with three arrivals/departures, 15 agent decisions, six completed objectives, three replans, and zero failed objectives or surface safety failures. This demonstrates policy execution over the imported Plan scenario; it does not independently validate the operational source records.

A separate live-update bridge covers a mapped Goal 33 route-closure observation while an adaptive simulator session is active. The Live updates form accepts `closure_started` / `closure_ended` for mapped routes. When the operator selects “Apply this mapped closure to a linked live simulator,” the versioned event is recorded in Goal 33, translated through the configured route mapping, and sent through the existing simulator operator-control path. The intervention log retains the source event ID and route. Duplicate source event IDs do not apply the intervention twice. A user advances the simulator to its next agent decision boundary to inspect the response. In the synthetic KAUO demonstration, event `fcd7e0f1-01d6-4dc9-a9df-59c5008ced02` closed the mapped taxiway at simulation second 1504; the ADAPTIVE agent replanned at second 1532 from `[6, 4, 3, 2]` to `[6, 7, 5, 4, 3, 2]`. Both aircraft objectives completed with no failed objectives, surface safety failures, or aircraft collisions. The imported schedule and geometry remain synthetic/approximate demonstration inputs, not evidence of actual KAUO operations. Evidence is in [goal33-live-update-replan](goal34-evidence/goal33-live-update-replan/) and its [real Studio screenshot](screenshots/goal34-goal33-live-update-replan.png).

## Goal 31 Compatibility

Goal 34 was developed from the completed Goal 33 baseline while Goal 31 remained incomplete in its separate worktree. It does not copy Goal 31's GSE routing, resource dispatch, turnaround execution, or deadlock recovery. Future GSE support can provide an agent objective, a typed observation, a deterministic policy implementing the decision interface, and actions mapped into the simulator's authoritative execution/safety path. Those interfaces can be integrated without embedding unfinished Goal 31 behavior in the Goal 34 policy layer.

## Known limitations

- The implemented production policy is aircraft surface routing; other agent types are architectural extension points.
- Adaptive behavior is based on deterministic route cost, closures, and sustained reservation blockage, not weather or probabilistic uncertainty.
- The KAUO schedule and approximate geometry are not operationally validated facts.
- The Goal 32 policy parameter, native two-policy experiment, and one/two-worker parity comparison are exercised; policy comparisons use one seed and do not establish multi-seed robustness.
- The timing comparison includes process startup, simulation, and artifact output; it is not a microbenchmark of policy evaluation alone.
