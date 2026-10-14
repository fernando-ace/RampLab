# Goal 43: KAUO route deadlock resolution

## Summary

The Goal 42 control trace does not show a reservation cycle. Aircraft 1 held the direct taxi edge while the 30 m aircraft to ground vehicle interlock held it behind an idle tug. The aircraft never reached a graph node, so it never reached the node-level reroute logic. Aircraft 2 then waited for the same edge.

The fix makes stationary ground vehicle clearance part of route feasibility before an aircraft enters an edge. When a safe alternate exists, the aircraft selects it. When the available geometry leaves no safe route, the aircraft stops at the runway exit, reports `NO_SAFE_ROUTE`, and releases the exit node so later arrivals can make their own safe decision.

The exact regenerated Goal 42 control scenario completes without a taxi deadlock or collision, but all three arrivals safely fail before taxiing. Its derived route graph has no 30 m clearance-safe path from the runway exit to the assigned gates with all nine GSE vehicles parked at their configured depots. This is a material limitation of the current generated geometry and vehicle placement, not evidence of successful KAUO surface operations.

## Diagnosis

The original Goal 42 `depot-fix-control` event stream records aircraft 1 acquiring edge 4 at simulated second 91. At second 120 it is at `(-218.052, -164)` and emits `SurfaceWaitingForTraffic` for vehicle 8 at `(-188, -156)`, whose state is `Idle`. Aircraft 1 never emits `SurfaceReservationReleased` for edge 4. Aircraft 2 later requests the same route and waits for that edge.

The aircraft to ground vehicle interlock correctly holds below 30 m; the collision threshold remains 8 m. `hold_for_clearance` advances both the active phase start and end by one second on each held tick. A stationary tug therefore prevents the taxi phase from completing. The aircraft is frozen between graph nodes, so existing node-level route replanning cannot resolve the hold. Aircraft 2 is blocked by aircraft 1's edge reservation; there is no reverse dependency from aircraft 1 to aircraft 2, so this is not a wait cycle.

## Implementation

- Centralized the existing 30 m aircraft to ground vehicle hold distance as `aircraft_ground_clearance_m`; the 8 m collision threshold is unchanged.
- Added deterministic point-to-centerline distance checks for idle vehicles with observed fleet poses.
- Excluded clearance-blocked edges from route search and route candidates, and report the blocking vehicle and edge in wait events.
- Made adaptive route selection prefer feasible candidates over invalid candidates. If no safe route remains, the policy fails the objective safely.
- Triggered immediate replanning when a stationary vehicle blocks the next edge, including for scripted movement.
- Released the runway-exit node reservation when an arrival has no safe route. This prevents a failed arrival from holding the exit and trapping subsequent arrivals in rollout.

No collision threshold or hold threshold was reduced. The simulator does not move an aircraft through a blocked edge.

## Regression evidence

`SurfaceTrafficTest.RoutesAroundStationaryGroundVehicleBeforeEnteringEdge` constructs a shorter direct edge crossing an idle GSE clearance area and a longer clear detour. Before the behavior change, the minimal scenario failed to finish within 500 advances: the aircraft remained `WaitingForTraffic` on the direct route. After the change, it selects the detour, arrives, and records zero aircraft to ground vehicle collisions. The test runs the fixture three times serially and compares complete event records.

`SurfaceTrafficTest.ArrivalWithNoReachableTaxiRouteFailsSafelyWithReason` now schedules a second arrival at the same runway exit. It verifies both aircraft report a safe failure, proving the first failure releases the exit node instead of leaving a stale reservation.

The full Release CTest suite passed: **222/222 tests**. This includes the repository's serial versus parallel determinism checks. The focused route regression was also run three times through CTest; all three passed.

## Exact Goal 42 control replay

Input: Goal 42 generated control scenario, seed 42, run against the Goal 43 Release CLI. The complete event stream and simulator metrics are under the ignored local path `results/goal43-exact-control-final/`.

| Measure | Result |
| --- | ---: |
| Simulated duration | 331 s |
| Wall execution time | 0.652 s |
| Event records | 33 |
| Aircraft | 3 |
| Safe surface failures | 3 |
| Completed arrivals / taxi routes | 0 / 0 |
| Aircraft to aircraft collisions | 0 |
| Aircraft to ground vehicle collisions | 0 |
| Fleet collisions / deadlocks | 0 / 0 |
| Unresolved service requests | 0 |
| Completed turnarounds | 0 of 3 |

Each arrival stops at the runway exit with `no available taxi route from runway exit to assigned arrival gate`. The aircraft do not acquire taxi edges. All nine ground vehicles are idle at their configured staging nodes in the generated scenario.

The generated graph has 94 aircraft-allowed edges. At the 30 m clearance, 24 of those edges intersect the clearance area of at least one idle vehicle. With those edges excluded, only five graph nodes are reachable from the runway exit, and none of the three assigned gates is reachable. For example, the direct `twy_a_mid_fbo_primary` and the `twy_c_fbo_detour` are both inside the clearance areas of parked vehicles. This is why a safe route cannot be selected for this exact control scenario. Resolving the remaining topology and staging placement requires a separate geometry review; no airport surface data was altered here.

The replay was run three times serially with the same scenario and seed. SHA-256 values were identical for all three runs:

- `events.jsonl`: `D228C62F03DC7537814E689A7877B71036962D3B40DCD848DB3413CB19C5EB2C`
- `simulator-metrics.json`: `62DDD0D518FE33F95ED0EAC7A69ED1E2DBC42B49DE483D71B4ED42C749D2C2C0`
- `simulator-metrics.csv`: `6F2E4AB3D404B6A85BFB6847F3D437651A45FE4FF9BE28426F94007758262A58`

## Commits and worktree

- Base: `1ae580b0ad63e24837fbecf07fc926cf744accd2`
- `f7a3bd8` — Avoid taxi routes blocked by idle ground vehicles
- `b51f8e4` — Release runway exit after safe arrival failure

The work is on branch `goal43-kauo-deadlock-resolution` in its dedicated worktree. The Goal 42 worktree was used only as a read-only source for the original trace and regenerated control scenario. No push, publication, merge, deployment, or pull request was performed.

## Limitations and next step

This change prevents the observed idle-GSE edge reservation deadlock and preserves conservative clearance behavior. The exact control demonstrates safe failure, not operational completion: all arrivals stop at the runway exit because the current generated network and idle vehicle locations leave no 30 m safe route. Review the KAUO route centerlines and GSE staging locations together, retaining their existing source and uncertainty labels, before claiming successful taxi feasibility. This remains a simulation result and does not validate real-world airport operations.
