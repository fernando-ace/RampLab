# Goal 44: KAUO routable surface topology

## Status

**Not complete. Runtime acceptance is blocked.** The source-backed Goal44 geometry is statically connected and the 30 m stationary-GSE clearance is retained for all nine configured depot positions. The native control run does not complete safely, and the scheduled closure disruption does not produce a usable alternate taxi route. The disruption run records aircraft and fleet collisions. These outputs do not satisfy the Goal44 completion criteria.

## Changes prepared

- Added a separate `kauo_goal44.geometry.json` and mapping. Goal42 geometry and Goal42/43 evidence files were not regenerated or overwritten.
- Added Goal44-only movement semantics. The 84 explicitly classified GSE, service, and stand-approach edges are ground-only. The ten aircraft-eligible edges are generated on the aircraft pavement raster buffered around all nine nodes classified as approximate GSE depots.
- Five connectors have no continuous TAXIWAY/APRON-only raster route. They remain unavailable to ground vehicles because connecting them would require an unverified runway crossing. The ALDOT pavement inventory does not grant that access.
- Added exact point-to-polyline depot-clearance, graph reachability, scheduled-closure, and topology-rendering tools. See [the topology diagnostic](goal44-evidence/topology-diagnostic.json) and [the rendered topology](goal44-evidence/topology.png).

## Static topology findings

The generated graph contains 94 edges. Ten are aircraft-eligible; all ten clear each of the nine attached depot positions by at least **38.184 m**, above the unchanged 30 m requirement. The three control flight assignments have static graph paths from `twy_a_mid` to their assigned stands and from each stand to `rwy36_hold`.

With the scheduled closure of `twy_a_mid_fbo_primary`, the graph search finds alternate paths of 3,370–3,390 m. Each alternative includes edges whose ALDOT inventory overlap includes runway pavement, and the data do not establish crossing authorization. These are graph paths, not verified operational routes. The simulator correctly cannot be credited with a successful disruption route from this static result.

The Goal43 baseline documented three safe arrival failures and 24 clearance-blocked aircraft edges in the previous topology. Goal44 removes the static depot-intersection failure on its ten aircraft-eligible routes, but the remaining graph and live fleet behavior still fail the end-to-end acceptance criteria. See [Goal43’s report](goal43-kauo-deadlock-resolution.md) for the unchanged baseline evidence.

## Native simulator evidence

The control replay reaches all three assigned gates and starts all three turnarounds. Its event stream records 20 completed service tasks, then the CLI exits with `dispatcher assigned a request without an available route` while returning vehicle 9 from the third gate’s water stand. The diagnostic build showed 30 vehicle-local graph links unavailable after runtime replanning, including both links between the third stand approach and the FBO hub. No completed control metrics were emitted, so the control run cannot be reported as a successful or collision-free run.

The scheduled-closure replay exits normally but is unsafe and incomplete: **0/3 aircraft departed, 0/3 turnarounds completed, three safe surface failures, three aircraft-to-aircraft collisions, one aircraft-to-ground collision, and 499 fleet collisions**. The fleet minimum separation was 0.063 m; the aircraft-to-ground minimum separation was 3.434 m. The run is retained as negative evidence, not as a passing result.

The exact final conservative run files are in `docs/goal44-evidence/runs/`. Earlier exploratory runs that temporarily enabled ground access across unverified runway surfaces were not accepted as evidence and did not change the final geometry policy.

## Validation

- Goal42 plus Goal44 focused geometry tests: **11 passed**.
- Airport scenario-generation package tests: **48 passed**.
- Airport data-ingestion tests: **13 passed**.
- Release CTest: **222/222 passed**.
- Control scenario generation repeated three times: `scenario.json`, `manifest.json`, `identity-map.json`, and `support-matrix.json` were byte-identical. Two Goal44 geometry builds were byte-identical.
- Both final generated scenarios pass `validate --generated`.
- Native Goal44 control and disruption acceptance: **failed**, as described above.

## Remaining evidence needed

Do not enable the five ground connectors or use the static closure alternates as operational routes without authoritative movement-area centerlines and runway-crossing permissions. A truthful Goal44 completion also requires resolving the runtime fleet-route invalidations and eliminating the observed collisions without changing the 30 m clearance or collision thresholds, then producing successful control/disruption runs and three byte-identical successful replays.

All work remains local on branch `goal44-routable-kauo-surface-topology`. Nothing was pushed, published, deployed, merged, or opened as a pull request.
