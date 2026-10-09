# KAUO-calibrated RampLab simulation prototype

This package demonstrates the Goal 24 source-to-simulation path using public KAUO information. It is a **KAUO-calibrated simulation prototype**, not a validated real-airport digital twin. The FAA chart provides airport/runway identity, dimensions and named airport areas. The simulator has a compact, approximate routing graph and one illustrative arrival/departure pair because surveyed surface geometry and event-level movement observations were not obtained.

Read [source provenance](../airport_data_ingestion/examples/kauo/PROVENANCE.md) and the [support/accuracy matrix](KAUO_SUPPORT_MATRIX.md) before interpreting results.

## Goal 26 registration correction

Goal 25's alignment error came from treating diagram-digitized runway centers and bearings as if they were geodetic control points. The approximate chart coordinates were combined with a nonzero 80 m east origin offset and a 95.7° local-grid heading, so geometry values no longer represented airport-local ENU directly. The transforms were individually valid for that declared legacy grid, but the runway centers and headings were estimates rather than measured endpoints. Against the FAA's surveyed runway-end records, the old geometry's four threshold errors range from 36.648 m to 66.832 m. Its runway headings were 185.7° and 112.0°, while the FAA endpoint-derived bearings are 181.040149° and 107.310813°.

The Cesium longitude/latitude ordering, negative-west longitude sign, Unreal north-to-negative-Y mapping, and meters-to-centimeters scale were not the source of the measured Goal 25 endpoint miss. That numerical miss came from chart-derived runway anchors and headings, with a hidden origin correction making the data frame harder to audit. The post-correction geometry uses the four FAA NASR runway-end coordinates directly. A separate Cesium runtime binding defect that affected what terrain appeared beneath the overlay is documented below.

| Goal 25 runway end | Error against FAA NASR endpoint |
|---|---:|
| Runway 18 | 65.850 m |
| Runway 36 | 66.832 m |
| Runway 11 | 36.648 m |
| Runway 29 | 63.271 m |

The repeatable rebuild command is `python -m tools.airport_scenario_generation.calibrate_kauo`. It reads [kauo.anchors.json](kauo.anchors.json), regenerates [kauo.geometry.json](kauo.geometry.json), and writes the independent quantitative artifact [kauo_alignment_validation.json](kauo_alignment_validation.json). Repeated rebuilds produce identical geometry and report bytes.

## Coordinate reference system and transformation

Geographic inputs use WGS 84 geographic 3D (EPSG:4979), with latitude/longitude in degrees and ellipsoid height in meters. The airport origin is the FAA NASR surveyed airport reference point, 32.61511111° N, 85.43400000° W. The scenario coordinate convention is airport-local ENU in meters: +X east, +Y north, +Z up. Geographic coordinates are converted through WGS-84 ECEF into the origin's topocentric ENU frame. The generic functions live in [geospatial.py](geospatial.py) and support the reverse conversion, true-bearing vectors, and explicitly declared legacy grids.

The conversion chain is:

```text
WGS 84 latitude / longitude / ellipsoid height
  -> WGS 84 ECEF
  -> airport-local ENU meters (+X east, +Y north)
  -> RampLab scenario XY (same ENU meters)
  -> Unreal centimeters (X = 100 * east, Y = -100 * north)
```

The Cesium georeference receives `FVector(longitude, latitude, ellipsoid height)` as required by its longitude/latitude/height API. The viewer origin now matches the FAA airport reference point. `SimulationHeadingDegrees=90` expresses that local +X is east; origin offsets are zero and scale is one. No KAUO-specific correction is applied in an actor. The runway overlay derives its segment direction from the FAA endpoint-derived true bearing.

## Unreal/Cesium visual registration verification

The runtime terrain tileset was originally spawned without an explicit georeference assignment. Cesium then used its default georeference resolution path, and the baseline validation view showed an unrelated terrain patch beneath the KAUO overlay. The airport environment now assigns its KAUO `ACesiumGeoreference` directly to the tileset before finishing its spawn. The runtime log checks and records that the resolved georeference is the expected actor and that its origin is `lon=-85.4340000, lat=32.6151111, height=208.22 m`. This removes reliance on choosing the first discovered georeference at runtime. Cesium documents that an unassigned tileset resolves the first georeference in the level or creates one if necessary ([Cesium georeference API](https://cesium.com/learn/cesium-unreal/ref-doc/classACesiumGeoreference.html)).

With the explicit binding, the Cesium imagery shows KAUO. In the final overhead and intersection captures, both generated runway boundaries follow the corresponding visible pavement, including the crossing. The apron outline falls within the visible terminal/FBO apron area. Its center and dimensions remain APPROXIMATE; this visual check does not upgrade them to surveyed coordinates. Taxiway paths remain chart-derived approximations and are not claimed to be pixel-accurate centerlines. No airport actor was moved manually in Unreal.

`-RampLabGeospatialWireframe` is a display-only capture option: it draws runway boundaries and the apron outline so the source imagery remains visible. It does not change scenario coordinates, actor placement, or simulation state. Camera capture presets are `KAUOOverview`, `KAUORunways`, `KAUOIntersection`, and `KAUOApron`.

| Capture | Evidence |
|---|---|
| Baseline entire airport overhead | [baseline-overview-final.png](../../results/goal26/kauo/unreal/baseline-overview-final.png) · [runtime log](../../results/goal26/kauo/unreal/baseline-overview-final.log) |
| Baseline runway crossing | [baseline-intersection-final.png](../../results/goal26/kauo/unreal/baseline-intersection-final.png) · [runtime log](../../results/goal26/kauo/unreal/baseline-intersection-final.log) |
| Baseline apron and taxiway | [baseline-apron-final.png](../../results/goal26/kauo/unreal/baseline-apron-final.png) · [runtime log](../../results/goal26/kauo/unreal/baseline-apron-final.log) |
| Disruption entire airport overhead | [disruption-overview-final.png](../../results/goal26/kauo/unreal/disruption-overview-final.png) · [runtime log](../../results/goal26/kauo/unreal/disruption-overview-final.log) |

FAA lists field elevation as 776.8 ft MSL. The configured 208.22 m ellipsoid height is derived using an approximate GEOID18 separation of -28.56 m; the FAA NASR horizontal coordinate is authoritative, while this ellipsoid height is approximate. Local feature registration is evaluated horizontally.

## Anchors, accuracy, and classification

| Anchor | Latitude | Longitude | Classification |
|---|---:|---:|---|
| Runway 18 end | 32.62020130° | -85.43603569° | AUTHORITATIVE, FAA NASR runway-end record |
| Runway 36 end | 32.60573677° | -85.43634563° | AUTHORITATIVE, FAA NASR runway-end record |
| Runway 11 end | 32.61958108° | -85.43735130° | AUTHORITATIVE, FAA NASR runway-end record |
| Runway 29 end | 32.61630966° | -85.42495000° | AUTHORITATIVE, FAA NASR runway-end record |
| Runway centerline intersection | 32.61923953° | -85.43605630° | DERIVED from the two FAA runway-end line pairs |
| Primary apron center | ENU (97.437 m east, 121.197 m south) | — | APPROXIMATE, chart-graticule reference |

The FAA NASR endpoint positions and runway dimensions are from the October 1, 2026 subscription. FAA identifies the endpoint position source as a third-party survey dated May 22, 2006, but does not publish a horizontal accuracy estimate in these fields. The 5 m project tolerance is therefore a visual-registration acceptance threshold, not a claim about survey accuracy. `kauo_alignment_validation.json` reports both endpoint coordinate round-trip residuals and errors for the generated runway shapes. The maximum generated runway-shape endpoint residual is 0.000007 m (rounding in six-decimal ENU output); all four endpoints pass the 5 m project tolerance. The same report preserves the measured Goal 25 baseline errors above.

Runway lengths and widths remain the authoritative published dimensions; the centerlines and bearings are derived from endpoint coordinates. The centerline intersection is DERIVED. Taxiway paths, apron, terminal/hangars, and the parking reference remain APPROXIMATE chart-digitized positions. No imagery-derived or chart-derived taxiway point is presented as surveyed. Scenario schedules, aircraft archetype, service task, taxi speed, and occupancy/rollout times remain ASSUMED exercise values.

The taxiway/apron/building features retain their prior FAA-diagram-informed relative layout after conversion from the declared Goal 25 frame into direct ENU. This preserves the approximate chart context while removing the old viewer correction. The FAA diagram does not provide surveyed taxiway centerlines or stand coordinates.

| FAA chart feature | RampLab identifier | Representation |
|---|---|---|
| Runway 18/36, 5,264 × 100 ft | `rwy18_threshold`, `rwy36_hold`, `rwy18_36_mid` | FAA NASR runway ends and dimensions; midpoint and true bearing are derived. The sim graph does not model pavement width. |
| Runway 11/29, 4,000 × 75 ft | `rwy11_threshold`, `rwy29_threshold`, `runway_11_29` geometry metadata | FAA NASR runway ends and dimensions; midpoint and true bearing are derived. |
| Runway intersection | `runway_intersection` | Derived line intersection of the four runway-end anchors. |
| Taxiway A west of runway 18/36 | `twy_a_*`, `twy_a_mid_fbo_primary` | Approximate topology follows the chart's west-side parallel taxiway and east-side terminal connection; not precise centerlines. |
| Terminal/FBO apron, east side | `fbo_apron_junction`, `ga_parking_stand` | One illustrative GA parking point; no published stand number or surveyed point is claimed. |
| West apron and self-serve fuel farm | `self_serve_fuel_farm` | Approximate chart-informed west-side feature anchor only; it is not a simulator vehicle spawn. |
| Taxiway C detour concept | `twy_a_north_fbo_detour`, `twy_c_fbo_detour` | Alternate approximate graph path used for closure stress testing. |

The graph uses Euclidean distances between its approximate chart-digitized node coordinates and edge traversal costs rounded at the declared calibration taxi speed. Each path is checked by native route finding at runtime. Closure of `twy_a_mid_fbo_primary` leaves a longer graph path through the north connector and apron detour. This is a model-level connectivity check, not an assertion that the exact route is available or authorized in actual airport operations.

## Operations and movement assumptions

KAUO is represented as a general-aviation/training airport with an FBO and documented fueling, not as a commercial airline terminal with jet bridges, baggage belts, catering loops, or airline stands. The canonical package includes one illustrative arrival/departure pair and an abstract pushback-preparation task. Its timestamps, representative single-engine aircraft archetype, parking point, task duration, and graph routing are assumptions; they are not observed KAUO movements. Auburn University publishes FBO fueling services, but this package does not simulate fueling because verified vehicle inventory, dispatch position, or aircraft-specific service durations were not found. The abstract task uses no vehicle resource. There are no observed flight IDs or actual airport operations in the package.

The graph's surface speed is 5.0 m/s (about 9.7 kt), a conservative assumed taxi speed for a compact GA sample. Fixed runway occupancy (90 s) and arrival rollout (90 s) are native-engine inputs chosen for a test, not measured KAUO values. The two-minute pushback-preparation task is an assumed abstract crew activity, not a measured ground service or vehicle dispatch. RampLab models event-based travel over edges and does not simulate aircraft acceleration or braking. No collision threshold was changed. Movement and service values need measured aircraft/airport observations before calibration claims can be strengthened.

## Build, generate, execute, analyze, replay, and package

Run from the repository root in PowerShell:

```powershell
$out = "results/goal25/kauo"
New-Item -ItemType Directory -Force -Path $out | Out-Null
python -m tools.airport_data_ingestion validate tools/airport_data_ingestion/examples/kauo/manifest.json --report "$out/validation.json"
python -m tools.airport_data_ingestion build tools/airport_data_ingestion/examples/kauo/manifest.json --output "$out/canonical.json"
python -m tools.airport_scenario_generation validate "$out/canonical.json" --mapping tools/airport_scenario_generation/mapping.kauo.json
python -m tools.airport_scenario_generation generate "$out/canonical.json" --mapping tools/airport_scenario_generation/mapping.kauo.json --output "$out/disruption"
python -m tools.airport_scenario_generation generate "$out/canonical.json" --mapping tools/airport_scenario_generation/mapping.kauo.json --output "$out/baseline" --without-disruptions
python tools/release/ramplab.py run --scenario-file "$out/baseline/scenario.json" --mode control --seed 42 --output "$out/runs/control"
python tools/release/ramplab.py run --scenario-file "$out/disruption/scenario.json" --mode disruption --seed 42 --output "$out/runs/disruption"
python tools/experiment_analysis/analyze.py "$out/runs/control/experiment.json" "$out/runs/disruption/experiment.json" --output "$out/analysis.md" --json "$out/analysis.json"
```

For repeats, execute the exact same two commands to separate output directories and use `tools.airport_scenario_generation.determinism` to compare metrics and ordered event logs. Create evidence with `python -m tools.evidence_bundle --control "$out/runs/control" --disruption "$out/runs/disruption" --determinism-report "$out/determinism.json" --output "$out/evidence"` after producing that report. Rebuild and inspect the geospatial artifact first with `python -m tools.airport_scenario_generation.calibrate_kauo`.

Goal 20 uses the generated run `experiment.json`, `aircraft.csv`, and `events.jsonl`. Start `python tools/ops_dashboard/server.py`, load those files, then inspect Event replay. Replay is the recorded event stream; it does not reconstruct positions independently. For Unreal, run the release runner with `--unreal` and the generated scenario file, on a host with the configured UE 5.8 toolchain. Verify the runtime log and visible scene separately; a successful headless run is not Unreal evidence.

## Validation and comparison limits

Validate package schema and provenance with Goal 24A; mapping and geometry references with Goal 24B; native operation completion, event routing and safety with Goal 23; KPI comparison with Goal 19; file loading and event replay with Goal 20; evidence bundle checks with Goal 22. Repeated same-seed generation should be byte-identical and repeated execution should match deterministic metrics/events. Report collisions and minimum separations from the actual run artifacts.

There is no event-level public observation in the package, so taxi distance/time, runway, parking, sequence, and turnaround comparison against real movements is **unavailable**. FAA charts support static feature comparisons only: runway dimensions/orientation and named taxi/apron features. The scenario's expected output cannot be treated as real-world ground truth. Useful next data would be a licensed/surveyed airfield GIS or centerline survey, verified stand/parking reference points, authorized time-stamped movement observations with runway/taxi path, aircraft class and actual service durations, and verified ground-equipment positions/capabilities.

Each run contains one aircraft and no ground vehicles. The zero collision counts describe only these modeled actors. Minimum aircraft-aircraft, aircraft-ground, and fleet separation are **not applicable** because there is no actor pair to measure; native `0.0` separation exports are not zero-meter clearances.
