# KAUO source provenance

Research/retrieval date: **2026-10-09**. The package is a calibration prototype, not an official airport database export.

| Source | Extracted information | Classification | Limits |
|---|---|---|---|
| FAA, [28 Day NASR Subscription, effective October 1, 2026](https://www.faa.gov/air_traffic/flight_info/aeronav/Aero_Data/NASR_Subscription/2026-10-01/), CSV subscriber file `APT_BASE.csv` | KAUO airport reference latitude 32.61511111° N, longitude 85.43400000° W, 776.8 ft field elevation; reference point position source and survey date | AUTHORITATIVE FAA published aeronautical data | FAA labels the position source as third-party survey dated May 22, 2006; this CSV field does not provide a horizontal accuracy estimate. |
| Same FAA NASR subscription, `APT_RWY.csv` and `APT_RWY_END.csv` | Runway 18/36 and 11/29 dimensions, runway-end latitude/longitude/elevation, true alignment, endpoint position source/date | AUTHORITATIVE FAA published aeronautical data | Endpoint position source is third-party survey dated May 22, 2006. Use the source coordinates and retain their uncertainty limits; do not treat their decimal precision as a published accuracy guarantee. |
| FAA, [KAUO Airport Diagram, cycle 2610 (effective October 1–28, 2026)](https://aeronav.faa.gov/d-tpp/2610/05127AD.PDF) | Runway and taxiway identity, airport feature names, runway dimensions, approximate relative taxiway/apron layout | AUTHORITATIVE chart for charted labels and published dimensions; diagram-digitized coordinates are APPROXIMATE | Not a centerline survey or stand-coordinate dataset. The chart graticule supports broad feature placement only. |
| FAA Southeast Chart Supplement, cycle 2026-03-19 (FAA publication): <https://aeronav.faa.gov/Upload_313-d/supplements/CS_SE_20260319.pdf> | Airport reference coordinates N32°36.91′ W85°26.04′; elevation 777 ft; 18/36 and 11/29 dimensions/surfaces; 100LL and Jet A service; attendance and airport communications | Authoritative published data, cycle-specific | This retrieved supplement is not the currently effective chart cycle at retrieval; consult current FAA publications and NOTAMs before operational use. The model does not use operating hours or communications. |
| Auburn University Regional Airport, official airport home page: <https://www.auburn.edu/administration/airport/> | Full-service FBO, Phillips 66 fuels, flight-training activity | Official public airport/operator information | Describes service and activity categories, not individual movements, traffic volumes, or a real schedule. |
| Auburn University Airport Services: <https://www.auburn.edu/administration/airport/services/> | Service/hours/contact page; tower/ground context | Official public airport/operator information | Web page is changeable; used only to characterize the site, not to set simulated operation timestamps. |
| Goal 25 project placement and chart anchors: `unreal/RampLabViewer/Config/DefaultGame.ini` and `tools/airport_scenario_generation/kauo.geometry.json` before Goal 26 | Legacy 32.6151667, -85.4340000 origin, 95.7° local +X bearing, 80 m east offset, approximate chart-digitized runway features | Legacy project assumptions; superseded for runway registration | Comparison to FAA NASR endpoints found 36.648–66.832 m runway-end error. Retained only as the declared source frame used to migrate approximate taxiway/apron features. |
| Goal 26 coordinate artifact: `tools/airport_scenario_generation/kauo.anchors.json` and `kauo_alignment_validation.json` | FAA runway-end geographic anchors, derived centerline intersection, ENU coordinates, and measured baseline/generated endpoint errors | Source-backed calibration and reproducible derived output | The 5 m overlay tolerance is a project acceptance threshold, not a guarantee of FAA survey accuracy. |

## Unavailable or not obtained

- No legitimate public event-level KAUO flight schedule/history with movement times, actual runway, taxi track, stand assignment, or turnaround timestamps was obtained. FAA traffic reporting references aggregate operations; it does not provide the observations needed to compare this scenario flight-by-flight.
- No surveyed stand/parking point inventory, taxiway centerline coordinates, pavement polygons, current apron road network, or vehicle dispatch location was obtained.
- Public sources identify fuel services and airport training activity, but no current fleet equipment inventory, per-aircraft fuel task duration, or service dispatch SLA was obtained.
- No authenticated source or paywalled feed was accessed. No live NOTAM or operational status is represented.

## Derived and assumed data in the package

- Coordinates from the FAA chart supplement are converted from degrees/minutes to decimal degrees. Feet are converted to meters with 0.3048 m/ft.
- The Goal 26 runway thresholds come from FAA NASR `APT_RWY_END.csv`; airport origin and runway dimensions come from `APT_BASE.csv` and `APT_RWY.csv`. The WGS-84-to-ENU calculation is deterministic; endpoint survey uncertainty remains as reported above.
- The runway lengths and widths in `kauo.geometry.json` are direct unit conversions from FAA-published feet. The charted layout's taxiway and apron arrangement informs a simplified approximate routing graph; graph anchors and stand point are estimates, not geodetic survey data.
- The two flight records are an illustrative paired GA arrival/departure at fixed UTC scenario times, not flights that occurred or a forecast. Aircraft type is a representative simulator archetype, not a KAUO tail number or observed aircraft.
- Canonical `airport.timezone` is `UTC` because every example timestamp is explicit UTC and this Windows standard-library environment has no IANA timezone database. `local_timezone` retains the documented KAUO civil zone (`America/Chicago`) without asking ingestion to interpret naive timestamps.
- The pair is assigned one two-minute abstract pushback-preparation task so the native turnaround workflow can complete without a mobile vehicle. Its duration is an assumed test parameter. Taxi speed, runway occupancy, arrival rollout, and pushback timing are also assumed. Fueling is intentionally not simulated: public sources establish fuel availability, but not vehicle inventory, dispatch location, or service duration.
- The taxiway connector closure exists only as a supported simulator stress case. It is not a reported or planned KAUO closure.

Raw input files are hashed into the Goal 24A canonical package. Goal 24B additionally hashes the canonical package, mapping, geometry, and generated outputs.


## Goal 27 illustrative operations addendum

The separate `kauo_goal27` source package contains two assumed representative GA aircraft, four illustrative UTC flight legs, two approximate parking positions, three assumed service units, eight assumed task requirements, and a controlled taxiway-to-apron closure. These rows do not describe actual or forecast KAUO operations, verified equipment inventory, surveyed stands, actual service procedures, or a reported/planned closure. Taxiway, apron, parking and staging coordinates remain APPROXIMATE/ASSUMED. See `docs/goal27-kauo-operational-digital-twin.md` for the integrated workflow and measured simulator outputs.
