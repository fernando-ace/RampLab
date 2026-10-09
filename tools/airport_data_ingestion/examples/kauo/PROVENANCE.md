# KAUO source provenance

Research/retrieval date: **2026-10-09**. The package is a calibration prototype, not an official airport database export.

| Source | Extracted information | Classification | Limits |
|---|---|---|---|
| FAA, current KAUO Airport Diagram, cycle 2610 (valid 2026-10-01 through 2026-10-29): <https://aeronav.faa.gov/d-tpp/2610/05127AD.PDF> | Runway labels/dimensions, runway orientation, taxiway labels A/A1/A2/A3/B/C/C2/C3, terminal/FBO apron, west apron, north hangar complex, self-serve fuel farm, field/runway-end elevations; chart graticule used to estimate relative feature placement | Authoritative published chart | Chart is a diagram, not surveyed taxiway centerline or parking coordinates. It does not expose stand IDs or vehicle spawn coordinates. |
| FAA Southeast Chart Supplement, cycle 2026-03-19 (FAA publication): <https://aeronav.faa.gov/Upload_313-d/supplements/CS_SE_20260319.pdf> | Airport reference coordinates N32°36.91′ W85°26.04′; elevation 777 ft; 18/36 and 11/29 dimensions/surfaces; 100LL and Jet A service; attendance and airport communications | Authoritative published data, cycle-specific | This retrieved supplement is not the currently effective chart cycle at retrieval; consult current FAA publications and NOTAMs before operational use. The model does not use operating hours or communications. |
| Auburn University Regional Airport, official airport home page: <https://www.auburn.edu/administration/airport/> | Full-service FBO, Phillips 66 fuels, flight-training activity | Official public airport/operator information | Describes service and activity categories, not individual movements, traffic volumes, or a real schedule. |
| Auburn University Airport Services: <https://www.auburn.edu/administration/airport/services/> | Service/hours/contact page; tower/ground context | Official public airport/operator information | Web page is changeable; used only to characterize the site, not to set simulated operation timestamps. |
| Airport-specific current Unreal placement: `unreal/RampLabViewer/Config/DefaultGame.ini` `[RampLab.AirportPlacement]` | Existing Cesium origin 32.6151667, -85.4340000, local +X bearing 95.7°, east offset 80 m, scale 1 | Existing project configuration | Reused as RampLab's georeference; it is not an independent survey or FAA source. |

## Unavailable or not obtained

- No legitimate public event-level KAUO flight schedule/history with movement times, actual runway, taxi track, stand assignment, or turnaround timestamps was obtained. FAA traffic reporting references aggregate operations; it does not provide the observations needed to compare this scenario flight-by-flight.
- No surveyed stand/parking point inventory, taxiway centerline coordinates, pavement polygons, current apron road network, or vehicle dispatch location was obtained.
- Public sources identify fuel services and airport training activity, but no current fleet equipment inventory, per-aircraft fuel task duration, or service dispatch SLA was obtained.
- No authenticated source or paywalled feed was accessed. No live NOTAM or operational status is represented.

## Derived and assumed data in the package

- Coordinates from the FAA chart supplement are converted from degrees/minutes to decimal degrees. Feet are converted to meters with 0.3048 m/ft.
- The runway lengths and widths in `kauo.geometry.json` are direct unit conversions from FAA-published feet. The charted layout's taxiway and apron arrangement informs a simplified approximate routing graph; graph anchors and stand point are estimates, not geodetic survey data.
- The two flight records are an illustrative paired GA arrival/departure at fixed UTC scenario times, not flights that occurred or a forecast. Aircraft type is a representative simulator archetype, not a KAUO tail number or observed aircraft.
- Canonical `airport.timezone` is `UTC` because every example timestamp is explicit UTC and this Windows standard-library environment has no IANA timezone database. `local_timezone` retains the documented KAUO civil zone (`America/Chicago`) without asking ingestion to interpret naive timestamps.
- The pair is assigned one two-minute abstract pushback-preparation task so the native turnaround workflow can complete without a mobile vehicle. Its duration is an assumed test parameter. Taxi speed, runway occupancy, arrival rollout, and pushback timing are also assumed. Fueling is intentionally not simulated: public sources establish fuel availability, but not vehicle inventory, dispatch location, or service duration.
- The taxiway connector closure exists only as a supported simulator stress case. It is not a reported or planned KAUO closure.

Raw input files are hashed into the Goal 24A canonical package. Goal 24B additionally hashes the canonical package, mapping, geometry, and generated outputs.
