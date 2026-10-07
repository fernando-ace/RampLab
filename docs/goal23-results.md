# Goal 23 release measurements

These figures were collected from the local seed-42 release run at `results/goal23/release-candidate/`. Native simulator JSON/CSV, per-aircraft records, and ordered JSONL events are the source. The output folder also contains the repeat-run comparison and complete Goal 22 bundle.

## Canonical mixed-runway comparison

The control scenario is `mixed_runway_operations`; the disruption scenario is `mixed_runway_disrupted`, which closes the A4 merge-to-gate link at simulation time 760 s.

| Measurement | Control | Disruption |
|---|---:|---:|
| Seed | 42 | 42 |
| Aircraft | 5 | 5 |
| Arrivals gated | 2/2 | 2/2 |
| Departures completed | 3/3 | 3/3 |
| Turnarounds completed | 3/3 | 3/3 |
| Required service tasks completed | 18/18 | 18/18 |
| Recorded closure events | 0 | 1 |
| Aircraft taxi distance | 998 m | 1,295 m |
| Aircraft taxi time | 1,946 s | 2,243 s |
| Departure taxi distance | 672 m | 672 m |
| Sum of departure delays | 2,696 s | 2,696 s |
| Runway operations completed | 5/5 | 5/5 |
| Runway utilization | 62.9496% | 62.9496% |
| Fleet reassignments | 0 | 0 |
| Aircraft-aircraft collisions | 0 | 0 |
| Aircraft-ground collisions | 0 | 0 |
| Minimum aircraft separation | 18.601633 m | 18.601633 m |
| Minimum aircraft-ground separation | 80.083478 m | 80.083478 m |
| Minimum fleet separation | 22.245595 m | 22.245595 m |

The affected arrival used the available longer route. Arrival taxi distance/time each increased by 297 m/seconds (326 to 623). The engine emitted no surface reroute event for this route chosen before taxi assignment; event replay preserves the emitted event history and the KPI comparison shows the taxi impact. Goal 19 safety comparison status was `no_regression`.

Repeated control metrics and ordered events were byte-identical. Repeated disruption metrics and ordered events were byte-identical. The JSON details are in `comparison/analysis.json`; the Goal 22 bundle includes this report as `evidence/determinism.json` and records its SHA-256.

## Separate vehicle-outage demonstration

The curated `vehicle-outage` scenario is `turnaround_flight_bank_outage`, seed 42. The ROS live probe observed all 3 turnarounds/departures, all 18 service tasks, and zero collisions. Vehicle 2 became unavailable at 130 s during task 3; vehicle 3 received the task at 130 s, started it at 168 s, and completed it at 408 s. This scenario's output does not contain the mixed-runway arrival/departure layer, so those KPIs are unavailable for this outage case. The CTest fleet outage and reassignment coverage also passed.

The native simulator run completed 3/3 turnarounds and all 18 tasks, with one reassignment, one disruption-triggered replan, no unresolved requests, and no fleet collisions. The fleet's minimum separation was 2.111018 m. Departure delays were 109 s (AX101), 58 s (AX202), and 19 s (AX303); all three completed late. AX101's reassigned baggage unload used `BaggageCart-Backup` and completed at 408 s. Native metrics and event records are under `vehicle-outage/`.

## Validation

- C++ Release build succeeded; CTest passed 179/179 tests.
- Python tests passed: Goal 19 analysis 10/10, Goal 20 dashboard/real bundle 16/16, Goal 22 evidence bundle 11/11.
- ROS 2 common, bridge, and controller test run passed 20/20 tests. The live outage topic probe observed three aircraft, 25 state samples, 108 ordered events, 18 task completion events, and the vehicle reassignment timings above.
- Unreal Engine 5.8 editor target build succeeded. A visible 1600×900 game window loaded `mixed_runway_disrupted` with seed 42 and reached the terminal runtime report: 2 arrivals, 3/5 departures, 5 runway operations, 623 m arrival taxi, 1,295 m total taxi distance, zero safe failures/collisions, and 18.602 m minimum aircraft separation.
- Goal 19 analysis of the real pair was comparable and reported `no_regression`. All four repeat checks were byte-identical.
- Goal 20 server analysis loaded the real control/disruption bundles, returned 196 and 199 recorded event rows, and analyzed the pair as comparable with `no_regression`. Its event stepper uses those JSONL records; comparing different scenarios naturally returns different event sequences.
- Goal 22 generated JSON, Markdown, static HTML, timeline, copied native artifacts, determinism report, manifest, and SHA-256 records. Bundle validation is `WARNING` solely because the two intentionally different scenario identities are compared; collision, minimum-separation, parse, consistency, determinism, and hash checks passed.

The main visible window/log, generated analysis, and bundle are under `results/goal23/release-candidate/`. Unreal's log is `unreal-windowed.log`; the static report is `evidence/index.html`.
