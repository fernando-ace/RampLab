# KAUO-calibrated RampLab simulation prototype

This package demonstrates the Goal 24 source-to-simulation path using public KAUO information. It is a **KAUO-calibrated simulation prototype**, not a validated real-airport digital twin. The FAA chart provides airport/runway identity, dimensions and named airport areas. The simulator has a compact, approximate routing graph and one illustrative arrival/departure pair because surveyed surface geometry and event-level movement observations were not obtained.

Read [source provenance](../airport_data_ingestion/examples/kauo/PROVENANCE.md) and the [support/accuracy matrix](KAUO_SUPPORT_MATRIX.md) before interpreting results.

## Coordinate and feature mapping

The airport reference point is 32.6151667° N, 85.4340000° W (the published N32°36.91′ W85°26.04′ converted to decimal degrees). Field elevation is 777 ft (236.8 m). The existing Cesium configuration in `DefaultGame.ini` supplies that georeference, 95.7° clockwise-from-north local +X heading, 80 m east origin offset, and scale 1.0. `RampLabAirportPlacement::ToUnreal` maps C++ airport-local XY positions through that transform. The calibration anchors below use the existing transform and the FAA diagram as a visual reference; they are approximate local-meter routing anchors and are not surveyed WGS-84 points.

The current FAA diagram's graticule was used to digitize approximate east/north offsets from the published reference point, then invert the existing placement heading and 80 m east offset to obtain RampLab-local XY. This makes the rendered arrangement follow the chart: runway 18/36 runs north-south, runway 11/29 crosses near its north end, Taxiway A is west of runway 18/36, the terminal/FBO apron is east, the self-serve fuel farm and west apron are west, and the north hangar complex is north. Chart pixels are approximate anchors; the diagram does not provide surveyed feature coordinates.

| FAA chart feature | RampLab identifier | Representation |
|---|---|---|
| Runway 18/36, 5,264 × 100 ft | `rwy18_36_mid`, `rwy36_hold` | Chart-graticule approximate reference and south-end departure queue node; published dimensions are stored in geometry metadata; the sim graph does not model pavement width. |
| Runway 11/29, 4,000 × 75 ft | `runway_11_29` geometry metadata | Approximate chart-graticule center and published dimensions; not used as an active route by this single-runway sample operation. |
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

For repeats, execute the exact same two commands to separate output directories and use `tools.airport_scenario_generation.determinism` to compare metrics and ordered event logs. Create evidence with `python -m tools.evidence_bundle --control "$out/runs/control" --disruption "$out/runs/disruption" --determinism-report "$out/determinism.json" --output "$out/evidence"` after producing that report.

Goal 20 uses the generated run `experiment.json`, `aircraft.csv`, and `events.jsonl`. Start `python tools/ops_dashboard/server.py`, load those files, then inspect Event replay. Replay is the recorded event stream; it does not reconstruct positions independently. For Unreal, run the release runner with `--unreal` and the generated scenario file, on a host with the configured UE 5.8 toolchain. Verify the runtime log and visible scene separately; a successful headless run is not Unreal evidence.

## Validation and comparison limits

Validate package schema and provenance with Goal 24A; mapping and geometry references with Goal 24B; native operation completion, event routing and safety with Goal 23; KPI comparison with Goal 19; file loading and event replay with Goal 20; evidence bundle checks with Goal 22. Repeated same-seed generation should be byte-identical and repeated execution should match deterministic metrics/events. Report collisions and minimum separations from the actual run artifacts.

There is no event-level public observation in the package, so taxi distance/time, runway, parking, sequence, and turnaround comparison against real movements is **unavailable**. FAA charts support static feature comparisons only: runway dimensions/orientation and named taxi/apron features. The scenario's expected output cannot be treated as real-world ground truth. Useful next data would be a licensed/surveyed airfield GIS or centerline survey, verified stand/parking reference points, authorized time-stamped movement observations with runway/taxi path, aircraft class and actual service durations, and verified ground-equipment positions/capabilities.

Each run contains one aircraft and no ground vehicles. The zero collision counts describe only these modeled actors. Minimum aircraft-aircraft, aircraft-ground, and fleet separation are **not applicable** because there is no actor pair to measure; native `0.0` separation exports are not zero-meter clearances.
