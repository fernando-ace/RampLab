# Goal 44: KAUO routable surface topology

## Status

**Not complete. Runtime acceptance is blocked.** The source-backed Goal44 geometry has statically reachable routes for all three arrivals and departures with the unchanged 30 m stationary-GSE clearance. Updated control and disruption replays on the regenerated geometry both fail turnaround completion and record fleet collisions/deadlocks. Static connectivity is not evidence of safe dynamic fleet operations.

## Changes prepared

- Added a separate `kauo_goal44.geometry.json` and mapping. Goal42 geometry and Goal42/43 evidence files were not regenerated or overwritten.
- Added Goal44-only movement semantics. The 84 explicitly classified GSE, service, and stand-approach edges are ground-only. The ten aircraft-eligible edges are generated on the aircraft pavement raster buffered around all nine nodes classified as approximate GSE depots.
- Five connectors have no continuous TAXIWAY/APRON-only raster route. They remain unavailable to ground vehicles because connecting them would require an unverified runway crossing. The ALDOT pavement inventory does not grant that access.
- Added exact point-to-polyline depot-clearance, graph reachability, scheduled-closure, and topology-rendering tools. See [the topology diagnostic](goal44-evidence/topology-diagnostic.json) and [the rendered topology](goal44-evidence/topology.png).
- Audited all 54 node attachments against the 2 m route raster. The mean snap displacement is **81.689 m** and the maximum is **230.143 m**; 46 attachments exceed one raster cell and 19 exceed 100 m. Stand 03 and its assumed service points account for the largest displacements. These inherited node locations remain unverified assumptions; snapping them to pavement does not establish their real-world locations.
- Corrected the machine-readable label `MANUALLY_RECONCILED_APPROXIMATION`: no manual survey or reconciliation supports these endpoints. They are now labeled `UNVERIFIED_APPROXIMATE_NODE_SNAP`, with source node classification and aggregate displacement metrics retained. The surface paths describe pavement-constrained geometry between assumed endpoints only.
- Added deterministic service-point attachment spacing: the 27 assumed GSE service points remain at least 4 m apart and at least 6 m from snapped service approaches. These distances are generation assumptions on classified pavement, not surveyed service-bay positions. The resulting replay reduces control fleet collisions from the earlier trace, but remains unsafe and incomplete; this adjustment does not qualify as a successful operational topology repair.

## Static topology findings

The regenerated graph contains 94 edges. Ten are aircraft-eligible; all ten clear each of the nine attached depot positions by at least **38.184 m**, above the unchanged 30 m requirement. The three control flight assignments have static graph paths from `twy_a_mid` to their assigned stands and from each stand to `rwy36_hold`. The current 54 node attachment audit reports mean displacement **81.689 m**, maximum **230.143 m**, 46 nodes beyond the 2 m raster resolution, and 19 beyond 100 m. The long snaps remain unverified endpoint assumptions.

With the scheduled closure of `twy_a_mid_fbo_primary`, the graph search finds alternate paths of 3,370–3,390 m. Each alternative includes edges whose ALDOT inventory overlap includes runway pavement, and the data do not establish crossing authorization. These are graph paths, not verified operational routes. The simulator correctly cannot be credited with a successful disruption route from this static result.

The Goal43 baseline documented three safe arrival failures and 24 clearance-blocked aircraft edges in the previous topology. Goal44 removes the static depot-intersection failure on its ten aircraft-eligible routes, but the remaining graph and live fleet behavior still fail the end-to-end acceptance criteria. See [Goal43’s report](goal43-kauo-deadlock-resolution.md) for the unchanged baseline evidence.

## Native simulator evidence

The original control replay reached all three assigned gates and started all three turnarounds, then exited with `dispatcher assigned a request without an available route` while returning vehicle 9 from the third gate’s water stand. Commit `140132f` resets request feasibility to the scenario graph with currently active road closures, avoiding that specific exception. The resulting control trace is still incomplete: it has 219 events through simulated second 4570, one aircraft ready for pushback, no departure event, and no final metrics. The run was stopped after memory use reached about 1.7 GB. This is not a passing or collision-free control run. The trace shows an unresolved fleet-return/turnaround dependency, but available exported events do not establish which return request or state transition is blocking progress. The post-reset trace and stop summary are retained under `goal44-evidence/runs/control-after-routing-reset/`.

The retained scheduled-closure replay from before the routing-graph reset is unsafe and incomplete: **0/3 aircraft departed, 0/3 turnarounds completed, three safe surface failures, three aircraft-to-aircraft collisions, one aircraft-to-ground collision, and 499 fleet collisions**. The fleet minimum separation was 0.063 m; the aircraft-to-ground minimum separation was 3.434 m. It is negative evidence, not a passing result. A post-reset disruption replay has not yet produced replacement final metrics.

The original conservative run files are in `docs/goal44-evidence/runs/`. Earlier exploratory runs that temporarily enabled ground access across unverified runway surfaces were not accepted as evidence and did not change the final geometry policy.

### Latest regenerated service-point-layout runs

The control and scheduled-closure disruption scenarios were regenerated from the same canonical operation package and the updated Goal44 geometry, then run with seed 42. The generated topology passed the route audit: all three arrivals and departure handoffs are statically reachable and minimum stationary-fleet clearance is 38.184 m.

- Control: **0/3** turnarounds completed; **403** fleet collision samples; minimum fleet separation **0.039 m**; **4** deadlocks, unresolved; **16/31** fleet requests completed and 15 failed. The three aircraft reached their gates, then entered `SafelyStopped` before departure.
- Disruption: **0/3** turnarounds completed; **13** fleet collision samples; minimum fleet separation **1.1 m**; **4** deadlocks, unresolved; **5/22** fleet requests completed and 17 failed.
- The control collision total fell from the earlier 6,100-sample replay, but both runs remain failed safety evidence. This change does not complete Goal44.

The updated run files are retained in `runs/control-service-layout/` and `runs/disruption-service-layout/`. Three seed-42 control replays produced byte-identical event streams, JSON metrics, metrics CSVs, and aircraft metrics CSVs. Their event-stream SHA-256 is `5222ab3ebe6f9ceadf08f97a141389c0e5d1529f6939acc1e51d705b8296526c`; the metrics SHA-256 is `4d4303d22d85e8b17b05c171459fffed327a8e2b35c858923a0977a639c5deb0`. Two consecutive Goal44 geometry regenerations were also byte-identical. These matching failed runs prove repeatability only, not safety or operational success.

## Validation

- Goal42 focused geometry tests: **8 passed**; updated Goal44 geometry and topology audit tests: **7 passed**; the complete `tools/airport_scenario_generation` package suite: **52 passed**.
- Airport scenario-generation package tests, imported as package modules: **49 passed**.
- Airport data-ingestion tests: **13 passed**.
- Release CTest: **223/223 passed** (latest full run before the Goal44-only Python attachment refinement; no C++ source changed afterward).
- Control scenario generation repeated three times: `scenario.json`, `manifest.json`, `identity-map.json`, and `support-matrix.json` were byte-identical. Two Goal44 geometry builds were byte-identical.
- Both final generated scenarios pass `validate --generated`.
- The Release `airside_cli` target rebuilt successfully after temporary diagnostic logging was removed; the executable SHA-256 is recorded in `determinism.json`. Clean-build control and disruption runs reproduced the recorded failure metrics.
- Goal44 evidence ZIP CRC and every internal SHA-256 ledger entry were verified; its adjacent archive sidecar records the ZIP SHA-256.
- Native Goal44 control and disruption acceptance: **failed**, as described above.
- Endpoint attachment audit: **54 nodes; mean 81.689 m; maximum 230.143 m; 46 beyond the 2 m grid resolution**. Long snaps are explicitly unverified approximations, not manual reconciliation.

## Remaining evidence needed

Do not enable the five ground connectors or use the static closure alternates as operational routes without authoritative movement-area centerlines and runway-crossing permissions. The current ALDOT inventory does not locate the assumed stands and service points closely enough to support operational endpoint claims. A truthful Goal44 completion also requires resolving the runtime fleet-return dependency and eliminating the observed collisions without changing the 30 m clearance or collision thresholds, then producing successful control/disruption runs and three byte-identical successful replays.

All work remains local on branch `goal44-routable-kauo-surface-topology`. Nothing was pushed, published, deployed, merged, or opened as a pull request.
