# Goal 38: Rolling-horizon coordinated surface planning

## Scope and evidence boundary

Goal 38 adds `ROLLING_HORIZON_COORDINATED` as a fourth aircraft surface routing policy. `SCRIPTED`, `ADAPTIVE`, and `COORDINATED_ADAPTIVE` remain separate policies. The planner only ranks routes already returned by the simulator's route candidate generator. Existing route closure handling, exclusive edge and node reservations, aircraft and ground-vehicle clearance, collision prevention, and safe failure behavior remain authoritative.

All Goal 38 scenarios are synthetic simulator fixtures. Geometry, schedules, speeds, and closures are controlled test inputs. The measurements below are not observations of real airport operations and do not establish airport-specific operational benefit.

## Architecture and cost model

The simulator provides each policy an immutable observation containing the active route, safe route candidates, current reservations, and a stable, bounded list of other aircraft's planned or forecast route edge uses. Ready aircraft without a plan use the deterministic shortest route as a forecast. The shared intent view is capped at 32 edge uses.

The rolling policy inspects at most the first eight edges and 600 simulated seconds of each candidate route. For every candidate edge in that horizon, it estimates the active aircraft's arrival and occupancy from distance and configured speed, then compares the time window with other aircraft's predicted edge arrival and occupancy. A ten second clearance margin expands each occupancy window. Overlap contributes a predicted delay to the active aircraft and a priority and wait-age adjusted delay to affected traffic.

Candidate cost is:

`taxi seconds + expected reservation wait * (1 + bounded own wait age / 600 + bounded objective priority / 20) + distance metres / 50 + predicted self delay + estimated delay imposed on other aircraft`

The delay imposed uses the existing bounded wait-age and objective-priority adjustment. These are engineering estimates, not airport-calibrated weights. The planner is a bounded route scorer, not a time-expanded global optimizer.

## Replanning and deterministic choices

An invalid route, closure, operator intervention, changed destination, or absent plan is handled immediately through the normal route-planning path. Otherwise, the policy compares the best candidate with the current route and replans only when the forecast cost improves by more than 15 seconds and the 10 second cooldown has elapsed. Reservation contention is considered in candidate scoring. Route candidate enumeration and aircraft IDs are stable; equal costs preserve candidate order. Movement arbitration continues to use wait age, objective priority, and stable aircraft ID.

Decision traces record the simulation timestamp, candidate action and route, local cost, predicted self and other delay, contention edge and both arrival estimates, horizon, chosen route, rejected candidate explanation, tie rule, replan trigger, and safety constraints. Closure and reservation feasibility still come from the simulator. A policy cannot reserve or traverse an unsafe edge by scoring it favorably.

## Evaluation methodology

`tools/goal38/run_matrix.py` runs four policies over ten controlled scenario roles and seeds 42, 43, and 44. It changes only `surface_operations.aircraft_routing_policy` in each configured scenario copy. Each serial run is repeated through the supported parallel replay and compared using normalized metrics and SHA-256 hashes of metrics, ordered events, and decision traces.

The roles include Goal 35 individual route choice, Goal 37 shared-route choice and early closure merge, shared bottleneck and staggered runway-entry arrivals, opposing arrival/departure traffic, congestion alternatives, closure during execution, a low-benefit baseline, and a difficult disrupted intersecting-traffic case. Inputs and their hashes are retained with the evidence. Metrics include taxi time and distance, waiting, objective completion/failure, departure delay, replans, reservation contention waits, minimum separation, collisions, and safety failures.

## Results

The matrix contains 120 serial runs and 120 parallel replays: ten scenarios, four policies, and three seeds. All normalized outputs and SHA-256 hashes matched. Each policy completed 108/108 objectives across its 30 runs. The entire matrix recorded zero collisions, zero surface safety failures, zero objective failures, and 18.602 m minimum aircraft separation.

The rolling policy did not improve fleet performance broadly. Equal weighted means across the ten scenarios were:

| Policy | Taxi s/run | Wait s/run | Distance m/run | Departure delay s/run |
|---|---:|---:|---:|---:|
| SCRIPTED | 1199.6 | 425.2 | 963.4 | 821.4 |
| ADAPTIVE | 1221.8 | 408.5 | 1002.6 | 828.9 |
| COORDINATED_ADAPTIVE | 1221.2 | 408.5 | 1002.6 | 828.9 |
| ROLLING_HORIZON_COORDINATED | 1221.2 | 408.5 | 1002.6 | 828.9 |

Rolling-horizon outcome against the best simpler policy by scenario (mean across three seeds):

| Scenario | Best simpler policy | Taxi s: simpler / rolling | Wait s: simpler / rolling | Distance m: simpler / rolling |
|---|---|---:|---:|---:|
| Shared bottleneck | SCRIPTED | 735 / 735 | 105 / 105 | 672 / 672 |
| Individual route choice | SCRIPTED | 941 / 1267 | 273 / 294 | 780 / 1056 |
| Shared route choice | COORDINATED_ADAPTIVE | 390 / 390 | 213 / 213 | 780 / 780 |
| Staggered runway entry | SCRIPTED | 735 / 735 | 195 / 195 | 672 / 672 |
| Long corridor congestion | ADAPTIVE | 1467 / 1467 | 354 / 354 | 1156 / 1156 |
| Opposing/intersecting traffic | SCRIPTED | 1946 / 1946 | 1137 / 1137 | 998 / 998 |
| Closure during execution | SCRIPTED | 800 / 800 | 64 / 64 | 768 / 768 |
| Little coordination benefit | SCRIPTED | 1861 / 1861 | 553 / 553 | 1861 / 1861 |
| Early closure merge | SCRIPTED | 768 / 768 | 32 / 32 | 768 / 768 |
| Difficult local choice | SCRIPTED | 2243 / 2243 | 1137 / 1137 | 1295 / 1295 |

By scenario, rolling-horizon matched the best of the three simpler policies in nine of ten cases. It matched the Goal 35 route-choice case's ADAPTIVE result, which was 326 seconds slower in taxi time than SCRIPTED. It did not improve the low-conflict, shared bottleneck, staggered arrival, mixed opposing traffic, closure, or difficult disrupted cases.

The only measured taxi-time reduction against ADAPTIVE was in the Goal 37 synthetic shared-route-choice fixture: six seconds (1.5%) in each of seeds 42–44. The reduction was limited to aircraft AX303's departure taxi time (106 to 100 seconds). Wait, route distance, departure delay, and completion remained unchanged. ROLLING_HORIZON_COORDINATED matched COORDINATED_ADAPTIVE exactly in this case, so the added lookahead provided no improvement beyond the immediate coordinator. This is a repeatable but narrow simulator result, not evidence of broad operational benefit or statistical significance.

Across all 30 rolling-horizon runs, the simulator reported 9 replans, 63 reservation contention waits, and 84 surface wait events. The structured traces contain the bounded horizon, candidate routes, predicted contention windows, replan trigger, tie rationale, and safety boundaries. Reservation wait counts come from ordered `SurfaceWaitingForTraffic` events. The portable bundle reports those counts by scenario and policy.

See [the Goal 38 evidence bundle](goal38-evidence/portable-bundle/README.md) for the aggregate comparison, per-run metrics and traces, serial/parallel replay checks, safety results, scenario copies, and hash ledger.

## Limitations

- Predictions extrapolate current planned or shortest forecast routes and constant configured speeds; they do not model uncertainty or future control changes beyond the known scenario state.
- The shared view and route horizon are intentionally bounded. Contention after eight edges or 600 seconds is not scored.
- The model approximates edge windows and delay; simulator reservations and safety logic make the actual movement decisions.
- The evaluation uses fixed synthetic scenarios and three deterministic seeds. It does not establish benefit under real-airport traffic distributions.
- Wait-age, priority, clearance, and improvement thresholds are not calibrated against operational data.
- In these scenarios, the new forecasts rarely changed the selected route. Added lookahead did not improve outcomes over `COORDINATED_ADAPTIVE`.

## Reproduction

From the repository root in a Visual Studio 2026 x64 environment:

```powershell
cmake -S . -B build-goal38 -G "Visual Studio 18 2026" -A x64 -DBUILD_TESTING=ON
cmake --build build-goal38 --config Release --target airside_tests airside_cli --parallel 6
ctest --test-dir build-goal38 -C Release --output-on-failure
python tools/goal38/run_matrix.py --cli build-goal38/Release/airside_cli.exe --output docs/goal38-evidence
python tools/goal38/build_bundle.py --evidence-root docs/goal38-evidence --output docs/goal38-evidence/portable-bundle
python -m unittest tools.experiment_analysis.test_analyze tools.evidence_bundle.test_bundle -v
python -m unittest tools.goal38.test_goal38 -v
```
