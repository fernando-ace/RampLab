# Goal 44: KAUO routable surface topology

## Status

**Not complete. Runtime acceptance is blocked.** The source-backed Goal44 geometry is statically connected and the 30 m stationary-GSE clearance is retained for all nine configured depot positions. The native control run does not complete safely, and the scheduled closure disruption does not produce a usable alternate taxi route. The disruption run records aircraft and fleet collisions. These outputs do not satisfy the Goal44 completion criteria.

## Changes prepared

- Added a separate `kauo_goal44.geometry.json` and mapping. Goal42 geometry and Goal42/43 evidence files were not regenerated or overwritten.
- Added Goal44-only movement semantics. The 84 explicitly classified GSE, service, and stand-approach edges are ground-only. The ten aircraft-eligible edges are generated on the aircraft pavement raster buffered around all nine nodes classified as approximate GSE depots.
- Five connectors have no continuous TAXIWAY/APRON-only raster route. They remain unavailable to ground vehicles because connecting them would require an unverified runway crossing. The ALDOT pavement inventory does not grant that access.
- Added exact point-to-polyline depot-clearance, graph reachability, scheduled-closure, and topology-rendering tools. See the [control topology diagnostic](goal44-evidence/topology-diagnostic.json), [disruption topology diagnostic](goal44-evidence/topology-disruption-diagnostic.json), and [rendered topology](goal44-evidence/topology.png).
- Audited all 54 node attachments against the 2 m route raster. The mean snap displacement is **80.743 m** and the maximum is **225.899 m**; 46 attachments exceed one raster cell and 19 exceed 100 m. Stand 03 and its assumed service points account for the largest displacements. These inherited node locations remain unverified assumptions; snapping them to pavement does not establish their real-world locations.
- Corrected the machine-readable label `MANUALLY_RECONCILED_APPROXIMATION`: no manual survey or reconciliation supports these endpoints. They are now labeled `UNVERIFIED_APPROXIMATE_NODE_SNAP`, with source node classification and aggregate displacement metrics retained. The surface paths describe pavement-constrained geometry between assumed endpoints only.

## Static topology findings

The generated graph contains 94 edges. Ten are aircraft-eligible; all ten clear each of the nine attached depot positions by at least **38.184 m**, above the unchanged 30 m requirement. The three control flight assignments have static graph paths from `twy_a_mid` to their assigned stands and from each stand to `rwy36_hold`.

The runtime-aligned topology audit now resolves the nine fleet positions from the committed control scenario, measures each aircraft-eligible polyline against those positions, and searches the undirected graph semantics used by `AirportGraph::find_route`. It finds safe static arrival and departure paths for all three flights, with **38.184 m** minimum clearance and no rejected aircraft edges. Arrival path lengths are 1,474.524–1,493.897 m; gate-to-runway-hold paths are 2,505.921–2,525.294 m. This is a preflight graph check against the fleet's configured starting positions. It does not prove live route safety after vehicles move, turnaround completion, or successful departures; the runtime failures below remain authoritative.

With the scheduled closure of `twy_a_mid_fbo_primary`, the graph search finds alternate paths of 3,370–3,390 m. Each alternative includes edges whose ALDOT inventory overlap includes runway pavement, and the data do not establish crossing authorization. These are graph paths, not verified operational routes. The simulator correctly cannot be credited with a successful disruption route from this static result.

The Goal43 baseline documented three safe arrival failures and 24 clearance-blocked aircraft edges in the previous topology. Goal44 removes the static depot-intersection failure on its ten aircraft-eligible routes, but the remaining graph and live fleet behavior still fail the end-to-end acceptance criteria. See [Goal43’s report](goal43-kauo-deadlock-resolution.md) for the unchanged baseline evidence.

## Native simulator evidence

The original control replay reached all three assigned gates and started all three turnarounds, then exited with `dispatcher assigned a request without an available route` while returning vehicle 9 from the third gate’s water stand. Commit `140132f` resets request feasibility to the scenario graph with currently active road closures, avoiding that specific exception. The resulting control trace is still incomplete: it has 219 events through simulated second 4570, one aircraft ready for pushback, no departure event, and no final metrics. The run was stopped after memory use reached about 1.7 GB. This is not a passing or collision-free control run. The trace shows an unresolved fleet-return/turnaround dependency, but available exported events do not establish which return request or state transition is blocking progress. The post-reset trace and stop summary are retained under `goal44-evidence/runs/control-after-routing-reset/`.

The retained scheduled-closure replay from before the routing-graph reset is unsafe and incomplete: **0/3 aircraft departed, 0/3 turnarounds completed, three safe surface failures, three aircraft-to-aircraft collisions, one aircraft-to-ground collision, and 499 fleet collisions**. The fleet minimum separation was 0.063 m; the aircraft-to-ground minimum separation was 3.434 m. It is negative evidence, not a passing result. A post-reset disruption replay has not yet produced replacement final metrics.

The retained fleet diagnostics localize the unresolved wait cycle at simulated second 2,928.24. Vehicle 2 (catering) waits for vehicle 7 at `intersection/6`; vehicle 7 (baggage load) waits for vehicle 2 at `edge/52`. In the loaded one-based graph, node 6 is `fbo_apron_junction` and edge 52 is the ground-only connection from that junction to the assumed stand-2 service approach. Recovery attempted vehicle 2 on both contested resources and emitted `retreat_failed`; the fleet then ended as `DEADLOCK_UNRESOLVED`. The evidence confirms a shared-apron dispatch cycle in this replay. It does not establish that this is the cause of the recorded collision count, and no recovery or geometry fix is claimed yet. See [the fleet deadlock diagnostic](goal44-evidence/fleet-deadlock-diagnostic.json).

The exact final conservative run files are in `docs/goal44-evidence/runs/`. Earlier exploratory runs that temporarily enabled ground access across unverified runway surfaces were not accepted as evidence and did not change the final geometry policy.

## Validation

- Goal42 focused geometry tests: **8 passed**; Goal44 focused geometry tests: **4 passed**.
- Airport scenario-generation package tests, imported as package modules: **49 passed**.
- Airport data-ingestion tests: **13 passed**.
- Release CTest: **222/222 passed**.
- Control scenario generation repeated three times: `scenario.json`, `manifest.json`, `identity-map.json`, and `support-matrix.json` were byte-identical. Two Goal44 geometry builds were byte-identical.
- Both final generated scenarios pass `validate --generated`.
- Goal44 evidence ZIP CRC and every internal SHA-256 ledger entry were verified; its adjacent archive sidecar records the ZIP SHA-256.
- Native Goal44 control and disruption acceptance: **failed**, as described above.
- Endpoint attachment audit: **54 nodes; mean 80.743 m; maximum 225.899 m; 46 beyond the 2 m grid resolution**. Long snaps are explicitly unverified approximations, not manual reconciliation.

## Remaining evidence needed

Do not enable the five ground connectors or use the static closure alternates as operational routes without authoritative movement-area centerlines and runway-crossing permissions. The current ALDOT inventory does not locate the assumed stands and service points closely enough to support operational endpoint claims. A truthful Goal44 completion also requires resolving the runtime fleet-return dependency and eliminating the observed collisions without changing the 30 m clearance or collision thresholds, then producing successful control/disruption runs and three byte-identical successful replays.

All work remains local on branch `goal44-routable-kauo-surface-topology`. Nothing was pushed, published, deployed, merged, or opened as a pull request.
