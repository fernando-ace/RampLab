# Goal 27: KAUO Operational Digital Twin

## Purpose and boundary

This package joins the Goal 24A airport-data ingestion, Goal 24B scenario generator, existing C++ operational simulator, Goal 26 KAUO registration, Goal 19 analysis, Goal 20 operator dashboard, Goal 22 evidence bundle, and the Unreal/Cesium viewer into one reproducible exercise.

It is a source-backed KAUO simulation demonstration. It is **not** a record or forecast of actual KAUO traffic, equipment, servicing, closures, turnaround performance, or safety. No event-level operational observations were found for validation against actual aircraft movements.

## Data flow

```text
FAA and Auburn public reference records + labeled simulation assumptions
  -> tools/airport_data_ingestion/examples/kauo_goal27/manifest.json and JSON tables
  -> Goal 24A canonical.json with source hashes and validation findings
  -> Goal 24B mapping + Goal 26 FAA runway geometry + approximate apron graph
  -> generated control/disruption scenario.json
  -> existing deterministic C++ simulation, snapshots, metrics, and events
  -> Goal 19 comparison + repeat-run determinism
  -> Goal 20 dashboard + Goal 22 evidence bundle
  -> Unreal mirrors the same scenario snapshots and events
```

The headless C++ simulator remains authoritative. Unreal reads the generated native scenario and mirrors simulation state. It does not script aircraft or vehicle movement.

## KAUO dataset and provenance

The source-controlled Goal 24A inputs are in `tools/airport_data_ingestion/examples/kauo_goal27/`; the generated canonical package is `tools/airport_data_ingestion/examples/kauo_goal27/canonical.json`. The separate directory keeps the Goal 25 one-aircraft prototype and its test contract intact. This Goal 27 package includes airport and runway metadata, operational-area labels, two aircraft archetypes, four illustrative flight legs, two illustrative parking resources, three assumed mobile equipment units, eight turnaround requirements, and a controlled route-closure definition. Goal 24A records source filenames, SHA-256 hashes, normalized records, and validation findings.

| Classification | Included information |
|---|---|
| Source-backed | FAA NASR airport reference point (32.61511111° N, 85.43400000° W), field elevation, runway dimensions, and four runway-end positions. FAA chart labels and public Auburn airport service categories are documented in `tools/airport_data_ingestion/examples/kauo/PROVENANCE.md`. |
| Derived | WGS 84 geographic 3D / EPSG:4979 to airport-local ENU conversion; runway centers, headings, intersection, graph-edge lengths, and source hashes. |
| Approximate | Taxiway graph, apron access, terminal-area context, and two illustrative parking positions. Taxiways, apron geometry, and service positions remain approximate where surveyed coordinates are unavailable. |
| Assumed for simulation | Flight identities and times, aircraft archetypes, two-gate occupancy, fuel/baggage/loading vehicle inventory and starting positions, service durations/dependencies, taxi speed and runway timings, and the test closure. These are not observed KAUO operations. |

The authoritative coordinate chain is unchanged: WGS 84 geographic coordinates / EPSG:4979 → airport-local ENU meters → Unreal centimeters (east maps to +X, north to -Y). The FAA airport reference point remains 32.61511111, -85.43400000. Runway thresholds continue to come from the Goal 26 FAA NASR geometry. There are no hidden offsets or KAUO-specific actor corrections.

The second illustrative parking node and three service staging nodes were added to `tools/airport_scenario_generation/kauo_goal27.geometry.json` for this two-aircraft exercise. Rebuild and verify that separate geometry with `python -m tools.airport_scenario_generation.calibrate_kauo --geometry tools/airport_scenario_generation/kauo_goal27.geometry.json --report tools/airport_scenario_generation/kauo_goal27_alignment_validation.json`. The calibration rebuild recomputes their distances and edge travel times while regenerating the same FAA runway thresholds. Their source records explicitly label them approximate or assumed. The Goal 25/26 `kauo.geometry.json` and its FAA alignment report remain unchanged.

## Actors and workflow

The generated scenario contains two representative general-aviation aircraft, two separate illustrative apron parking resources, and three active simulated vehicles: one fueling unit, one baggage-service unit, and one loading-service unit. Equipment presence and dispatch positions are scenario assumptions; they are not a claim about KAUO's inventory.

Each aircraft has an arrival, gate occupancy, turnaround, and scheduled departure. Services are serialized: fueling, baggage unload, baggage load, then abstract pushback preparation. After a task, each autonomous mobile unit returns to its assigned staging stand before the aircraft can push back. The Goal 27 scenario explicitly enables `surface_operations.return_service_vehicles_to_depot_after_task`; it defaults off for other scenarios. These return legs are modeled fleet requests, reducing gate congestion and preventing an aircraft departure from conflicting with service equipment left at the stand. Mobile service requests use the existing fleet dispatcher, route graph, vehicle motion, collision checks, and reservation behavior. The surface layer coordinates aircraft taxi and shared runway use.

The disruption case closes the mapped approximate primary Taxiway A-to-apron connector at simulation time 15 s and reopens it at 2,280 s. This is a controlled stress scenario, not a KAUO NOTAM or reported closure. The alternative path is already present in the Goal 26 graph. The measured route/delay effect, fleet response, and safety fields are reported below from actual run exports.

## Run commands

Use PowerShell from the repository root. The one entry point supports a complete pair, either individual case, an explicit seed, and optional visible Unreal/dashboard launches:

```powershell
cmake -S . -B build-goal27 -G "Visual Studio 18 2026" -A x64
cmake --build build-goal27 --config Release --parallel 4
ctest --test-dir build-goal27 -C Release --output-on-failure

python tools/release/ramplab.py kauo --mode pair --seed 42 --build-dir build-goal27
python tools/release/ramplab.py kauo --mode control --seed 42 --output results/goal27/kauo/control-only --build-dir build-goal27
python tools/release/ramplab.py kauo --mode disruption --seed 42 --output results/goal27/kauo/disruption-only --build-dir build-goal27
```

The pair command generates the canonical package and both scenarios, validates the generated packages, runs control and disruption plus one same-seed repeat for each, writes Goal 19 comparison and determinism reports, and creates the Goal 22 bundle. Seed 42 is the reference. The output root defaults to `results/goal27/kauo/seed-42` and must be new or empty.

Optional integrations run from the same pair output:

```powershell
python tools/release/ramplab.py kauo --mode pair --seed 42 --build-dir build-goal27 --unreal
python tools/release/ramplab.py kauo --mode pair --seed 42 --build-dir build-goal27 --dashboard
```

Each run directory contains native `simulator-metrics.json/.csv` and ordered `events.jsonl`, plus `experiment.json`, `runs.csv`, `aircraft.csv`, and `release-run.json`. `generated/` contains control/disruption `scenario.json`, generation `manifest.json`, `identity-map.json`, and `support-matrix.json`. The pair adds `analysis.md/.json`, `determinism.json`, and `evidence/` with source copies, hashes, KPI comparison, timeline, validation, Markdown, JSON, and standalone HTML report.

## Control and disruption results

All entries below are filled from the real seed-42 outputs under `results/goal27/kauo/seed-42-verified/`, not dashboard fixtures. They describe simulation observations only.

| Measure | Control | Disruption | Evidence field / interpretation |
|---|---:|---:|---|
| Aircraft completed / failed | 2 / 0 | 2 / 0 | Native surface and turnaround metrics |
| Arrivals / departures completed | 2 / 2 | 2 / 2 | Native surface metrics |
| Turnarounds completed; service tasks completed | 2; 8/8 | 2; 8/8 | Two required turnaround flows, four tasks each |
| Taxi distance and time | 3,737.69 m / 752 s | 5,765.20 m / 1,160 s | Total aircraft taxi distance and time |
| Arrival taxi distance / time | 934.45 m / 188 s | 2,961.96 m / 596 s | Detour adds 2,027.51 m and 408 s in this run |
| Runway waiting | 2 s total; 0.50 s avg | 1 s total; 0.25 s avg | Native runway queue metrics |
| Turnaround duration / departure delay | 20.40 min / 18.32 min avg | 34.18 min / 34.58 min avg | Goal 19 comparison aggregates |
| Ground-vehicle travel / waiting | Not exported per unit | Not exported per unit | Fleet records show 12/12 requests complete, 0 failures, 0 reassignments in each run |
| Reroutes / reassignments / recovery | 0 / 0 | 0 / 0 | The disruption route was selected at initial planning while the closure was active; no post-assignment reroute was needed |
| Collisions / minimum separation | 0; aircraft-ground 43.89 m, aircraft-aircraft 236.98 m, fleet 2.05 m | 0; aircraft-ground 43.97 m, aircraft-aircraft 289.51 m, fleet 2.05 m | Aircraft, aircraft-ground, and fleet separation evidence reported separately; safety failures 0 |

Goal 19 comparison and its available KPI set are in `analysis.md` and `analysis.json`. A missing recovery or vehicle-distance KPI is explicitly unavailable; it is not inferred from event proximity. Safety interpretation is limited to the simulation's recorded actors and available separation fields.

## Determinism and validation

The pair workflow repeats the same seed-42 control and disruption executions and compares native KPI/run records and ordered event streams. `determinism.json` follows the Goal 22 four-check repeat contract. Generated control and disruption scenarios share the same canonical source package, mapping, geometry, epoch, and seed; only the controlled road-event application differs.

The Goal 27-specific tests are `python -m unittest tools.airport_scenario_generation.test_kauo_goal27 -v`. The complete pair and same-seed repeats are under `results/goal27/kauo/seed-42-verified/`; all four metric/event comparisons are byte-identical. Goal 19 reports 50 shared numeric KPIs, with no safety regression and an added 2,027.51 m / 408 s of taxiing in the disrupted case. These are synthetic-case results. ROS is not modified by this goal; Unreal captures and dashboard checks are reported separately from headless simulation.

## Unreal workflow and evidence

Unreal uses `-RampLabScenarioFile=<generated scenario.json>` and the same C++ loader. Goal 26 keeps explicit Cesium georeference binding and logs the resolved KAUO origin at startup. The operational layer uses simulation snapshots/events; approximate taxi/apron lines and simple vehicle/aircraft meshes are context only. The visible playback override `-RampLabPlaybackSpeed=<0.1..60>` changes presentation timing only. `-RampLabScreenshotDelaySeconds=<seconds>` positions the automated capture in the simulation timeline when paired with that speed.

With `--unreal`, the entry point builds the Unreal target once, then launches three visible views: a control runway view at 1x, a disruption apron view at 5x, and a disruption completion view at 60x. PNGs and corresponding runtime logs are written under `results/goal27/kauo/seed-42-verified/unreal/`. The dashboard loads native metrics, aircraft/task CSV, and event JSONL, then presents overview, KPIs, aircraft, disruptions, safety, event replay, and analysis. Goal 22's static bundle is usable offline.

## Known limitations

- Taxiway routes, apron geometry, gate/parking positions, and service staging remain approximate where surveyed coordinates are unavailable.
- Flight times, actor identities/archetypes, equipment, dispatch locations, durations, and closure are explicit simulator assumptions, not actual or historical KAUO records.
- RampLab surface movement is event-based and its ground vehicles use the existing fixed-step model; this does not model actual aircraft or vehicle dynamics.
- No event-level KAUO movement history, surveyed taxiway/stand dataset, verified service fleet, or operational validation observations were obtained.
- The dashboard replays recorded events; it does not independently reconstruct physical movement. Goal 19 comparison is limited to metrics actually exported, and the Goal 22 safety report is not a real-airport safety determination.
