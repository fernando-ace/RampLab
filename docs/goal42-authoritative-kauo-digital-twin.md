# Goal 42: KAUO surface geometry and event replay

## Executive summary

Goal 42 adds 37 georeferenced Alabama DOT pavement inventory polygons for KAUO, explicit provenance, a deterministic transformation into the existing KAUO ENU frame, a graph-overlap diagnostic, timestamped position fields on simulator events when the simulator has a pose, an event-to-Unreal coordinate adapter, and a Goal 42 Scenario Studio profile. The matched Goal 41 style control/disruption runs completed with 3/3 turnarounds and 33/33 tasks, zero reported collision or safety-failure counters, and byte-identical repeated event files.

This is a simulation prototype. Goal 42 does **not** establish airport certification, operational accuracy, safe real-world autonomous movement, or an airport digital twin suitable for operations. The route graph still uses Goal 31's approximate centerlines; the new pavement inventory is not wired as a movement constraint. The requested full visual Dashboard and Unreal replay correlation was not achieved in this environment.

## Geometry provenance

| Geometry | Source and classification | Limits |
|---|---|---|
| Runway-end references | FAA NASR `APT_RWY_END.csv`; authoritative source records, carried forward from Goal 26/31. | Source survey date is 2006-05-22; decimal precision is not an accuracy guarantee. |
| Runway centerlines/intersection | Derived from FAA runway endpoint records. | Computed geometry, not independent survey. |
| Runway, taxiway, apron inventory extents | Alabama DOT Aeronautics Bureau Airport Pavement Management GIS, layer `Section_241016`, `NETWORKID=AUO`; source-backed-derived polygons; acquired 2026-10-14. Original layer CRS is Web Mercator WKID 102100; query returned WGS 84 EPSG:4326. | Pavement inventory extents are not certified movement boundaries, taxiway centerlines, current operating status, or legal routing. The layer describes a 2018 study with hosted edits through 2024-10-17. Its 2 apron polygons do not establish individual stands. |
| Route graph | Goal 31 FAA-diagram-informed topology and node/edge abstractions; derived with approximate centerlines. | Preserved for deterministic compatibility. It has not been snapped to ALDOT features or made subject to surface-polygon constraints. The nearest-surface diagnostic now reports sampled distances for all 94 edges and separately for production-used routes; see Geometry mismatch investigation. These checks are not proof of legal or safe routing. |
| Gates/stands | Illustrative scenario mapping; approximate. | No surveyed stand coordinates were obtained. |
| GSE service/staging points | Goal 41 illustrative configuration; approximate. | Inventory, service times, depots, and service access are assumptions. |

Source references: [ALDOT Airport Pavement Management Program](https://www.dot.state.al.us/programs/AirportPavementMaintenance.html), [KAUO GIS feature layer](https://services.arcgis.com/LZzQi3xDiclG6XvQ/arcgis/rest/services/Section_241016/FeatureServer/0), and [FAA Airport Diagram Open Data notice](https://www.faa.gov/air_traffic/flight_info/aeronav/safety_alerts/media/ADDS_25-01_DPN_Open_Data_Release.pdf). The FAA open-data notice does not list KAUO in the published initial airport coverage, which is why ALDOT is used for pavement extents.

The downloaded source GeoJSON and layer metadata are included under `docs/goal42-evidence/sources/`. Source snapshot SHA-256: `5327a009231bfe3352e6473caf2cf8e4ec46a92bfc15e6ed2efc002b8a9e6f89`.

## Geometry mismatch investigation

The earlier 2-of-94 overlap count was a coarse diagnostic: it sampled straight endpoint chords for all graph edges, including assumed GSE spokes and service connectors, and counted only edges with at least 90% of samples inside taxiway/apron outer rings. The new 101-point nearest-distance audit measures every stored edge and the production-used edges separately, using the existing ENU coordinates unchanged and without relaxing a tolerance. The overlay is `docs/goal42-evidence/geometry/geometry-reconciliation-overlay.png`; exact per-edge values are in `docs/goal42-evidence/geometry/geometry-reconciliation.json`.

| Diagnostic | Measured result |
|---|---:|
| Route graph edges / nodes | 94 / 54 |
| Classified ALDOT operating-surface features / polygon parts | 31 / 33 |
| All-edge maximum nearest-surface distance, median / mean / p95 / max | 70.087 / 80.388 / 188.209 / 190.664 m |
| Edges whose maximum sampled distance is <=1 m / <=10 m / <=25 m | 2 / 6 / 25 of 94 |
| Production route edges used in control event history | 6 unique edges, 18 segment assignments |
| Production-edge maximum nearest-surface distance, median / mean / max | 49.692 / 58.339 / 161.424 m |
| Production edges whose maximum sampled distance is <=1 m / <=10 m | 1 / 1 of 6 |

The `twy_a_south_mid` edge is closely aligned (maximum sampled distance 0.084 m), and `rwy36_hold` is 0.324 m from the ALDOT Runway 18-36 inventory. In contrast, the straight `twy_a_mid_fbo_primary` chord reaches 48.776 m from classified operating pavement; the runway-hold-to-A connector reaches 50.608 m; and the assumed Gate 02 / Gate 03 edges reach 67.885 m / 161.424 m. Gate 02 and Gate 03 nodes themselves are 67.885 m and 161.424 m from classified pavement; Gate 01 is inside Apron 01. This pattern points to localized inherited graph chords and assumed stand positions, rather than one airport-wide origin, rotation, or axis-sign offset. It is a consistency diagnosis against a 2D inventory, not a survey-accuracy finding.

The [FAA cycle 2610 diagram](https://aeronav.faa.gov/d-tpp/2610/05127AD.PDF), effective 2026-10-01 through 2026-10-29, names Taxiways A and C, A1/A2/A3, C2/C3, the terminal/FBO apron, and the west apron. It supports relative topology but is diagrammatic, not a survey centerline source. The FAA runway anchors and the ALDOT runway and Taxiway A polygons align; the inherited graph collapses named connector topology into straight chords and assumes gate locations. The FAA PDF and its checksum manifest are included under `docs/goal42-evidence/sources/`.

Currentness is also unresolved. The ALDOT GIS snapshot was last edited 2024-10-17. Auburn University's [April 2024 trustee project record](https://www.auburn.edu/administration/trustees/materials/materials-20240412.pdf) described a north terminal-ramp expansion (base project: 8,760 sq yd and 15 tie-down spaces). ALDOT's [June 2026 grants notice](https://www.aldotnews.com/2026/06/16/alabama-airport-improvement-grants-total-2-million/) lists “Expand Terminal Area (North) (Phase 5)” and “Expand Terminal Area Apron (North) (Phase 6).” These records show the 2024 pavement snapshot cannot be called a current as-built north-apron model; they provide no georeferenced/as-built geometry. Source records and limits are preserved in `docs/goal42-evidence/sources/`.

## Coordinate system

The source query returns WGS 84 longitude/latitude (EPSG:4326) from a Web Mercator (WKID 102100) layer. Independent FAA runway-end anchors lie 0.000 m, 0.000 m, 0.825 m, and 0.324 m from their nearest ALDOT runway polygons in this local frame. The recorded checks are included in `geometry-reconciliation.json`; the polygon boundary and FAA endpoint records are not a substitute for an airport survey. Each point is transformed with the existing `geospatial.py` WGS-84 ECEF-to-local-topocentric conversion into ENU meters, using the KAUO ARP `(32.61511111°, -85.434°, 208.22 m ellipsoid height)`. East is +X, north is +Y, and the source has no vertical coordinate, so pavement elevation remains unknown. The ARP and runway references follow the existing FAA NASR source records; the ellipsoid height is derived as documented in Goal 26 calibration metadata.

For Unreal world coordinates, the existing viewer convention is `X = east × 100 cm`, `Y = -north × 100 cm`, `Z = 0 cm` for 2D source pavement. Heading converts as `Unreal yaw = -heading_rad × 180/π`. The replay adapter applies this conversion and retains source ENU values; it does not interpolate between samples.

## Architecture

```text
FAA NASR runway endpoints + ALDOT pavement inventory
  -> Goal 42 normalized geometry and provenance
  -> existing Goal 31 route graph + Goal 41 operation scenario
  -> C++ simulator snapshots and timestamped event records
  -> Dashboard event list / selected-event pose display
  -> JSONL event-to-Unreal transform adapter
```

The Goal 42 geometry profile is consumed by the existing scenario generator and Scenario Studio map. Its pavement polygons are carried into generated scenario metadata, while C++ movement continues to use the existing graph nodes and edges. This is a material gap: movement has not yet been constrained or rerouted against the ALDOT surface areas.

Canonical event records retain sequence, simulation timestamp, typed event, stable numeric and named entity identifiers, event/task/edge context, state, and position/orientation when available from the simulator snapshot at event emission. Missing poses remain absent. The Python adapter emits Unreal coordinates only for records with a complete pose and reports pose-less records instead of inferring positions. On the disruption run it produced 121 aircraft and 72 ground-vehicle event samples; the control run produced 121 aircraft and 73 ground-vehicle samples. All nine vehicle IDs appear across each run, but most service-task or vehicle records remain pose-less.

## Scenario and measured results

Seed 42, 3 illustrative aircraft, 9 mobile ground units, 33 required turnaround tasks. The disruption is the Goal 41 assumed pushback service delay for `KAUO-G31-GA-03`: at simulation second 241, extend pushback from 180 to 480 seconds. It is not an observed airport condition.

| Measure | Control | Disruption |
|---|---:|---:|
| Completed turnarounds | 3/3 | 3/3 |
| Arrivals / departures completed | 3 / 3 | 3 / 3 |
| Required tasks completed | 33/33 | 33/33 |
| Taxi distance | 5,877.353 m | 5,877.353 m |
| Taxi time | 1,188 s | 1,182 s |
| Departure delay total | 643 s | 937 s |
| Aircraft-aircraft collisions | 0 | 0 |
| Aircraft-ground collisions | 0 | 0 |
| Ground fleet collisions | 0 | 0 |
| Configured safety failures / failed tasks | 0 | 0 |

Both matched runs and their repeats passed the Goal 41 workflow's safety, completion, evidence-bundle, and determinism checks. Repeated control and disruption event streams were byte-identical. The geometry profile retains the same 94-edge approximate routing graph, so Goal 42 does not claim geometry-caused routing changes or compare these metrics as a geometry improvement.

## Replay correlation

The simulator JSONL event schema now provides point-in-time poses for aircraft and mobile-fleet events where the simulator snapshot has a pose. The Dashboard's selected event panel shows stable entity ID, simulation time, ENU position, and heading. Goal 42's `goal42_replay.py` converts the same source event samples into Unreal coordinate rows with event, entity, edge and task identity intact. Adapter tests cover known ENU-to-Unreal values, missing poses, and non-monotonic timestamps.

The configured Goal 42 disruption also completed through the simulator-backed `LiveSession` API: it started paused and finished at simulated time 4,409 seconds with 3 aircraft, 9 vehicles, 33 tasks, 314 events, and analysis artifacts. This demonstrates the Python control path against the real CLI, not browser Start/Resume/Finish interactions. The session summary and artifacts are included in the evidence bundle.

Four selected disruption-run samples (arrival, departure runway clearance, the 241-second pushback disruption, and G31-FUEL-01 service) correlate by event sequence and stable entity ID. For all four samples, the Dashboard replay payload and simulator event use the same ENU point; converting through Unreal coordinates and back measured 0.0 m error at a 0.00002 m tolerance. This checks the transform and event identity, not the browser renderer. The existing Unreal subsystem still advances an in-process simulation or reads live snapshot state; it does not consume this recorded-event adapter. The environment lacks the enabled CesiumForUnreal plugin, so Unreal Editor build and visual replay validation could not be completed. Do not interpret adapter output as proof that Unreal rendered the run. The Dashboard browser review also could not be completed: Browser Harness could open a new tab, but Chrome's CDP `Runtime.evaluate` calls repeatedly timed out after 5 seconds, including on a blank tab. Scenario Studio API bootstrap and automated tests pass, but visual/interactive browser criteria remain unverified.

## Safety and validation

- CMake Release configure and `airside_cli`/`airside_tests` build: completed.
- CTest: full Release suite passed, 219/219, including `MovementEventsCarryAuthoritativePoseAndStableEntityIdentity`.
- Python suites passed: airport ingestion 13; scenario generation/geospatial/Goal 42 44; experiment analysis 10; Dashboard/Scenario Studio 76 (7 documented skips); evidence bundle 13. One Dashboard export integration test was skipped because its test harness expects a CLI at its default path; the Goal 41 workflow separately exercised the built CLI for control, disruption, and repeats.
- Goal 42 geometry and replay adapter: focused suite passes (13 tests).
- JavaScript syntax checks: Dashboard and Scenario Studio scripts pass `node --check`.
- Goal 41 vertical workflow with the Goal 42 geometry mapping: matched scenarios generated and validated, runs/repeats completed, bundle validation PASS, event determinism PASS.
- Goal 42 configured disruption completed through the simulator-backed LiveSession API with analysis artifacts; browser UI remains unverified.
- Browser-rendered Dashboard and Scenario Studio interactions: not verified because Chrome CDP evaluation timed out.
- Unreal Editor: not verified because enabled CesiumForUnreal is missing from the installed UE 5.8 plugin tree.

Final rerun results and complete test counts are recorded in the portable evidence README and `docs/goal42-evidence/validation.json`.

## Limitations

- Simulation only; schedules, aircraft types, equipment, task requirements, service durations, and disruption timing are illustrative assumptions.
- The pavement dataset is an inventory, not certified taxiway geometry or real-time operational status; its 2024 vintage also predates north-apron work phases reported by ALDOT in 2026.
- Route centerlines, stands, service points, and GSE operations remain derived or approximate.
- The simulator still routes on the previous graph; ALDOT polygons currently improve context and provenance rather than governing movement.
- No airport certification, airport deployment, real-airport validation, or autonomous-vehicle performance claim is made.
- The Unreal adapter is independently testable, but a Cesium-enabled editor and event-file playback actor remain needed to demonstrate actual recorded-event rendering.
- The Dashboard browser replay could not be visually validated on this Chrome session.

## Recommended Goal 43

Obtain a traceable KAUO taxiway centerline and stand dataset from an authoritative source, reconcile it with the 94-edge graph under explicit topology review, and make the simulator reject or flag edges outside the allowed movement graph. Then add a Cesium-compatible recorded-event playback reader in Unreal and validate the same event file in a responsive browser/editor environment with measured entity-position agreement. Preserve a reviewable separation between source samples and visual interpolation.
