# Goal 50: Synthetic Demo Airport Foundation

## Status and decision

**Status: INCOMPLETE.** The canonical C++ operations scenario, typed service-road permissions, repeatable evidence, regression tests, and documentation are in place. The engine run is operational and safe under its modeled constraints. The airport still lacks authoritative-by-construction runway pavement/threshold geometry and a fully aligned render model, so this work does not claim the complete, visually renderable airport requested by Goal 50.

The environment is fictional and is intentionally separate from the evidence-constrained KAUO environment. It makes no claims about real airports and does not change KAUO readiness semantics.

## Purpose and layout

RampLab Demo Airport is a compact regional-airport scenario for exercising engine-owned arrivals, runway queues, exits, taxi routing, stand assignment, turnarounds, mobile servicing, pushback, departure taxi, and runway sequencing. The canonical schedule uses five aircraft and five stands:

| Flight | Operation | Stand | Scheduled arrival | Scheduled departure |
|---|---|---|---:|---:|
| AX101 | Stand-start turnaround | A1 | 0 s | 400 s |
| AR401 | Arrival and turnaround | A4 | 400 s | 1,600 s |
| AR502 | Arrival and turnaround | A5 | 500 s | 1,800 s |
| DEMO202 | Stand-start turnaround | A2 | 5,000 s | 6,500 s |
| DEMO303 | Stand-start turnaround | A3 | 5,500 s | 7,000 s |

The graph includes a depot, north/south junctions, three nearby stands, two remote stands, two arrival exit connectors, a remote taxi merge and alternate branch, stand-service positions, and a five-stand apron. A1–A3 share the central taxi network; A4/A5 use longer paths and share junction infrastructure. Two shared vehicles (one fueling truck, one baggage cart) create measurable contention.

The operational definition is [ramplab_demo_airport.yaml](../scenarios/ramplab_demo_airport.yaml). Its C++ engine graph is the operational source of truth. The `airport.design` block carries inspectable synthetic assumptions for local coordinates, stand dimensions/compatibility, support-area semantics, route widths, and future Unreal unit conversion. The current scenario loader ignores that design block; tests inspect its values directly, while engine routing consumes the graph nodes, edges, and movement permissions.

## Authored assumptions and simulator model

The definition records a local right-handed metric frame, +X east, +Y north, +Z up, with the depot node as local origin. Unreal conversion is deterministic (`1 m = 100 cm`) and needs no geospatial origin or Cesium. Authored values include a 1,400 m by 30 m asphalt runway, 18 m taxiway corridors, 7 m GSE-only service spurs, 52 m by 45 m stands with 36 m maximum wingspan compatibility, and 8 m wing clearance. These are synthetic design assumptions, not survey measurements.

Edges tagged `service_road` or `apron_service_spur` are GSE-only in the loaded graph, each with a positive authored width; aircraft taxi links retain aircraft access. The graph contains no runway-crossing edge. The simulator models runway use as a timed resource (`runway_node`, arrival exits, occupancy, and queues), rather than a traversable pavement edge. That is an existing simulator constraint, not a claim that physical runway geometry is complete.

Logical support areas include GSE depot, fuel staging, maintenance/overflow staging, and terminal/apron locations. They are semantic graph locations, not building footprints. Engine-derived route geometry is the edge/node graph and edge distances/traversal times. Widths, stand clearances, surface labels, and named support metadata are authored design annotations and are not yet runtime clearance calculations.

## Canonical scenario and observed result

The canonical scenario uses seed 50, five aircraft, five stands, two service vehicles, two arrivals, five departures, six required turnaround tasks per aircraft, and three mobile service requests per aircraft. It exercised arrival rollouts and exits, shared-resource waiting, baggage/fuel tasks, turnaround dependencies, pushback, taxi, and runway queues through `airside::Simulation` and the `airside_cli` adapter.

Release CLI result:

| Metric | Result |
|---|---:|
| Aircraft / completed turnarounds | 5 / 5 |
| Arrivals completed / departures completed | 2 / 5 |
| Required turnaround tasks completed | 30 / 30 |
| Mobile service requests completed | 15 / 15 |
| Simulated duration | 11,507 s |
| Mean departure delay | 2,834.8 s (47.2 min) |
| Total runway operations / max queue depth | 7 / 2 |
| Runway queue wait | 551 s |
| Fleet collisions / minimum fleet separation | 0 / 2.14773 m |
| Unresolved fleet deadlocks | 0 |
| Surface safe failures / failed route objectives | 0 / 0 |
| Aircraft-aircraft / aircraft-ground collisions | 0 / 0 |

All five aircraft were delayed. This is expected to be a high-contention product demo fixture, not a capacity or schedule-performance claim. The definition uses only two service vehicles across five overlapping turnarounds.

## Validation and repeatability

Commands run from the Goal 50 worktree:

```powershell
cmake -S . -B build-goal50 -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build-goal50 --config Release --parallel 8
ctest --test-dir build-goal50 -C Release --output-on-failure
./build-goal50/airside_cli.exe --scenario scenarios/ramplab_demo_airport.yaml --seed 50 --quiet --metrics-json evidence/goal50/metrics.json --record-events evidence/goal50/events.jsonl
python tools/goal50/render_demo_airport.py
```

The diagram command uses Python with PyYAML installed.

Results: Release build succeeded; **232/232 CTest tests passed**, including three new Demo Airport tests for fictional coordinate metadata, explicit service-road permissions and graph connectivity, and repeated lifecycle safety. A second clean CLI run produced byte-identical metrics and JSONL event artifacts. The focused deterministic test also ran the scenario twice and compared engine event histories and metrics. The topology diagram generator was run twice and produced byte-identical SVG output.

Evidence is under [evidence/goal50](../evidence/goal50): `metrics.json`, `events.jsonl`, `airport-topology.svg`, and `manifest.json`. The event trace is about 96 KiB. The manifest records SHA-256 hashes for both runs, the scenario definition, and the diagram.

## KAUO boundary

KAUO remains the evidence-backed research environment with unresolved production routing data and `DATA_BLOCKED` semantics. Demo Airport is fictional, fully authored for its represented graph and operating assumptions, and must not be used to promote or fill in KAUO geometry, permissions, clearances, or readiness.

## Remaining work and limitations

- The engine graph is a coarse operational network inherited from the existing lifecycle scenario. The runway is a timed simulator resource, not an airport-local polygon/centerline with threshold nodes connected into taxi topology.
- Surface widths, stand clearance, and compatibility are metadata checks; the engine does not perform runtime width/wingtip-clearance validation from those fields.
- Support areas are logical nodes, not complete apron, terminal, depot, or maintenance footprints. The generated SVG is an operational topology view only; it does not draw runway pavement or support-area footprints.
- Aircraft and GSE share taxiway corridors where the scenario labels them shared-use; the simulator's existing GSE collision checks and typed service-spur restrictions remain enabled.
- The airport needs a geometry-complete source definition aligned with the operational graph, plus validation for runway thresholds/exits, hold points, dimensions, and rendered pavement, before it can be called fully renderable.
- The two-vehicle schedule produces a 47.2-minute mean departure delay. A later product goal may tune traffic or policy, while preserving the current high-contention scenario as a deterministic fixture.

## Recommended next step

Add a geometry-complete airport definition whose runway thresholds, taxiway centerlines, apron, stands, support footprints, hold points, and local route graph agree by construction; load or validate that definition against the engine graph; then export a deterministic plan view for Unreal. Keep all operational decisions in the C++ engine and keep the KAUO evidence/readiness gate unchanged.
