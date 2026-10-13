# RampLab Experiment Analysis: airport_kauo_goal41

## Experiment overview

- Source: `C:\Users\Ferna\.codex\worktrees\goal41-production-airport-operations-vertical\RampLab\docs\goal41-evidence-final\control\experiment.json`
- Identity: `{"experiment_name": "airport_kauo_goal41", "ordinal": 1, "run_count": 1, "schema_version": "1.0", "seed": 42, "source_scenario": "airport_kauo_goal41"}`
- Metrics available: 50

- Comparison: `airport_kauo_goal41` (`C:\Users\Ferna\.codex\worktrees\goal41-production-airport-operations-vertical\RampLab\docs\goal41-evidence-final\disruption\experiment.json`)

## KPI comparison

| KPI | Baseline | Comparison | Change | Impact |
|---|---:|---:|---:|---|
| Aircraft count | 3.000 count | 3.000 count | 0.000 count (+0.0%) | unchanged |
| Arrival runway wait | 1.000 s | 1.000 s | 0.000 s (+0.0%) | unchanged |
| Arrival taxi distance | 1,537.084 m | 1,537.084 m | 0.000 m (+0.0%) | unchanged |
| Arrival taxi time | 309.000 s | 309.000 s | 0.000 s (+0.0%) | unchanged |
| Average runway wait | 97.000 s | 97.000 s | 0.000 s (+0.0%) | unchanged |
| Mean departure delay | 3.572 min | 5.206 min | 1.633 min (+45.7%) | regression |
| Mean service waiting | 5.712 min | 5.712 min | 0.000 min (+0.0%) | unchanged |
| Mean turnaround | 49.756 min | 51.422 min | 1.667 min (+3.3%) | regression |
| Completed turnarounds | 3.000 count | 3.000 count | 0.000 count (+0.0%) | unchanged |
| Delayed aircraft | 3.000 count | 3.000 count | 0.000 count (+0.0%) | unchanged |
| Delayed turnarounds | 3.000 count | 3.000 count | 0.000 count (+0.0%) | unchanged |
| Departure runway wait | 581.000 s | 581.000 s | 0.000 s (+0.0%) | unchanged |
| Departure taxi distance | 4,340.269 m | 4,340.269 m | 0.000 m (+0.0%) | unchanged |
| Departure taxi time | 879.000 s | 873.000 s | -6.000 s (-0.7%) | improvement |
| Disruption replans | 0.000 count | 1.000 count | 1.000 count | regression |
| Failed or timed out turnarounds | 0.000 count | 0.000 count | 0.000 count | unchanged |
| Fleet collisions | 0.000 count | 0.000 count | 0.000 count | unchanged |
| Fleet minimum separation | 1.970 m | 1.970 m | 0.000 m (+0.0%) | unchanged |
| Outstanding reservations | 0.000 count | 0.000 count | 0.000 count | unchanged |
| Fleet reassignments | 0.000 count | 0.000 count | 0.000 count | unchanged |
| Fleet requests completed | 48.000 count | 48.000 count | 0.000 count (+0.0%) | unchanged |
| Fleet requests created | 48.000 count | 48.000 count | 0.000 count (+0.0%) | unchanged |
| Fleet requests failed | 0.000 count | 0.000 count | 0.000 count | unchanged |
| Fleet reservation contentions | 67,851.000 count | 67,851.000 count | 0.000 count (+0.0%) | unchanged |
| Unfinished fleet requests | 0.000 count | 0.000 count | 0.000 count | unchanged |
| Peak taxiing aircraft | 2.000 aircraft | 1.000 aircraft | -1.000 aircraft (-50.0%) | regression |
| Maximum runway queue | 1.000 aircraft | 1.000 aircraft | 0.000 aircraft (+0.0%) | unchanged |
| Maximum service task wait | 1,414.000 s | 1,414.000 s | 0.000 s (+0.0%) | unchanged |
| Minimum aircraft-ground separation | 13.565 m | 31.550 m | 17.985 m (+132.6%) | improvement |
| Minimum aircraft separation | 151.031 m | 948.133 m | 797.102 m (+527.8%) | improvement |
| Road closure events | 0.000 count | 0.000 count | 0.000 count | unchanged |
| Runway operations completed | 6.000 count | 6.000 count | 0.000 count (+0.0%) | unchanged |
| Runway queueing | 582.000 s | 582.000 s | 0.000 s (+0.0%) | unchanged |
| Runway utilization | 0.131 ratio | 0.122 ratio | -0.009 ratio (-6.8%) | regression |
| Simulation duration | 4,109.000 s | 4,409.000 s | 300.000 s (+7.3%) | regression |
| Aircraft collisions | 0.000 count | 0.000 count | 0.000 count | unchanged |
| Aircraft-ground collisions | 0.000 count | 0.000 count | 0.000 count | unchanged |
| Completed arrivals | 3.000 count | 3.000 count | 0.000 count (+0.0%) | unchanged |
| Completed departures | 3.000 count | 3.000 count | 0.000 count (+0.0%) | unchanged |
| Surface reroutes | 0.000 count | 0.000 count | 0.000 count | unchanged |
| Surface safety failures | 0.000 count | 0.000 count | 0.000 count | unchanged |
| Total taxi distance | 5,877.353 m | 5,877.353 m | 0.000 m (+0.0%) | unchanged |
| Total taxi time | 1,188.000 s | 1,182.000 s | -6.000 s (-0.5%) | improvement |
| Surface aircraft | 3.000 count | 3.000 count | 0.000 count (+0.0%) | unchanged |
| Surface wait events | 1.000 count | 0.000 count | -1.000 count (-100.0%) | improvement |
| Surface queueing | 1,129.000 s | 1,123.000 s | -6.000 s (-0.5%) | improvement |
| Task reassignments | 0.000 count | 0.000 count | 0.000 count | unchanged |
| Service task queueing | 11,310.000 s | 11,310.000 s | 0.000 s (+0.0%) | unchanged |
| Total turnarounds | 3.000 count | 3.000 count | 0.000 count (+0.0%) | unchanged |
| Unresolved service requests | 0.000 count | 0.000 count | 0.000 count | unchanged |

## Operational impact

- **Improvement: Minimum aircraft separation** — +797.102 m.
- **Improvement: Minimum aircraft-ground separation** — +17.985 m.
- **Regression: Disruption replans** — +1.000 count.
- **Improvement: Surface wait events** — -1.000 count.
- **Regression: Peak taxiing aircraft** — -1.000 aircraft.
- **Regression: Mean departure delay** — +1.633 min.
- **Regression: Simulation duration** — +300.000 s.
- **Regression: Mean turnaround** — +1.667 min.
- **Regression: Runway utilization** — -0.009 ratio.
- **Improvement: Departure taxi time** — -6.000 s.

## Safety

Assessment: **no_regression**
- Zero collisions recorded in both runs.

## Disruption and recovery observations

Available disruption, reroute, reassignment, completion, and failure fields are included in the KPI comparison. Recovery duration is not exported by the current schema.

## Determinism


## Conclusion

Compared 50 shared numeric KPI(s). 10 ranked change(s) are shown above.\n
