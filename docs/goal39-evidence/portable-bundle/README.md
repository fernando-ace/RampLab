# Goal 39 portable evidence bundle

This bundle contains controlled synthetic simulator evidence. It does not represent measured airport operations.

- Scenarios: 10
- Seeds: 42, 43, 44
- Serial runs: 150
- Parallel replay runs: 150
- Determinism: PASS
- Collisions: 0; safety failures: 0; objective failures: 0
- Minimum aircraft separation: 18.602 m

## Counterfactual comparison

- Changed-decision matched cases: 27
- Outcomes after a changed decision: improved 21, worsened 3, no effect 3.
- Candidate rollouts: 699; private model events: 2097.
- Different actions or routes from same-observation ROLLING_HORIZON_COORDINATED: 63/387 (16.3%).
- Mean complete CLI runtime by policy (seconds): SCRIPTED 0.3293; ADAPTIVE 0.3276; COORDINATED_ADAPTIVE 0.3288; ROLLING_HORIZON_COORDINATED 0.3299; COUNTERFACTUAL 0.3291
- Runtime is a whole-process overhead proxy; deterministic event/rollout counts are the reproducible work measure.

## Scenario and policy results

Each row summarizes three seeds. Mean taxi and wait are seconds per run; distances are metres per run.

| Scenario | Policy | Mean taxi s | Mean wait s | Mean distance m | Objectives complete | Failures | Replans | Surface wait events | Reservation waits |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| shared_bottleneck | SCRIPTED | 735.00 | 105.00 | 672.00 | 9 | 0 | 0 | 6 | 3 |
| shared_bottleneck | ADAPTIVE | 735.00 | 105.00 | 672.00 | 9 | 0 | 0 | 6 | 3 |
| shared_bottleneck | COORDINATED_ADAPTIVE | 735.00 | 105.00 | 672.00 | 9 | 0 | 0 | 6 | 3 |
| shared_bottleneck | ROLLING_HORIZON_COORDINATED | 735.00 | 105.00 | 672.00 | 9 | 0 | 0 | 6 | 3 |
| shared_bottleneck | COUNTERFACTUAL | 799.00 | 63.00 | 768.00 | 9 | 0 | 0 | 6 | 3 |
| individual_route_choice | SCRIPTED | 941.00 | 273.00 | 780.00 | 9 | 0 | 0 | 9 | 6 |
| individual_route_choice | ADAPTIVE | 1267.00 | 294.00 | 1056.00 | 9 | 0 | 15 | 15 | 15 |
| individual_route_choice | COORDINATED_ADAPTIVE | 1267.00 | 294.00 | 1056.00 | 9 | 0 | 9 | 15 | 15 |
| individual_route_choice | ROLLING_HORIZON_COORDINATED | 1267.00 | 294.00 | 1056.00 | 9 | 0 | 0 | 15 | 15 |
| individual_route_choice | COUNTERFACTUAL | 1549.00 | 166.00 | 1388.00 | 9 | 0 | 0 | 18 | 18 |
| shared_route_choice | SCRIPTED | 396.00 | 213.00 | 780.00 | 9 | 0 | 0 | 3 | 3 |
| shared_route_choice | ADAPTIVE | 396.00 | 213.00 | 780.00 | 9 | 0 | 0 | 3 | 3 |
| shared_route_choice | COORDINATED_ADAPTIVE | 390.00 | 213.00 | 780.00 | 9 | 0 | 3 | 3 | 0 |
| shared_route_choice | ROLLING_HORIZON_COORDINATED | 390.00 | 213.00 | 780.00 | 9 | 0 | 0 | 3 | 0 |
| shared_route_choice | COUNTERFACTUAL | 390.00 | 213.00 | 780.00 | 9 | 0 | 0 | 3 | 0 |
| staggered_runway_entry | SCRIPTED | 735.00 | 195.00 | 672.00 | 9 | 0 | 0 | 6 | 3 |
| staggered_runway_entry | ADAPTIVE | 735.00 | 195.00 | 672.00 | 9 | 0 | 0 | 6 | 3 |
| staggered_runway_entry | COORDINATED_ADAPTIVE | 735.00 | 195.00 | 672.00 | 9 | 0 | 0 | 6 | 3 |
| staggered_runway_entry | ROLLING_HORIZON_COORDINATED | 735.00 | 195.00 | 672.00 | 9 | 0 | 0 | 6 | 3 |
| staggered_runway_entry | COUNTERFACTUAL | 799.00 | 99.00 | 768.00 | 9 | 0 | 0 | 6 | 3 |
| long_corridor_congestion | SCRIPTED | 1571.00 | 542.00 | 1040.00 | 9 | 0 | 0 | 9 | 9 |
| long_corridor_congestion | ADAPTIVE | 1467.00 | 354.00 | 1156.00 | 9 | 0 | 15 | 15 | 15 |
| long_corridor_congestion | COORDINATED_ADAPTIVE | 1467.00 | 354.00 | 1156.00 | 9 | 0 | 9 | 15 | 15 |
| long_corridor_congestion | ROLLING_HORIZON_COORDINATED | 1467.00 | 354.00 | 1156.00 | 9 | 0 | 0 | 15 | 15 |
| long_corridor_congestion | COUNTERFACTUAL | 1549.00 | 166.00 | 1388.00 | 9 | 0 | 0 | 18 | 18 |
| opposing_intersecting_traffic | SCRIPTED | 1946.00 | 1137.33 | 998.00 | 15 | 0 | 0 | 15 | 12 |
| opposing_intersecting_traffic | ADAPTIVE | 1946.00 | 1137.33 | 998.00 | 15 | 0 | 0 | 15 | 12 |
| opposing_intersecting_traffic | COORDINATED_ADAPTIVE | 1946.00 | 1137.33 | 998.00 | 15 | 0 | 0 | 15 | 12 |
| opposing_intersecting_traffic | ROLLING_HORIZON_COORDINATED | 1946.00 | 1137.33 | 998.00 | 15 | 0 | 0 | 15 | 12 |
| opposing_intersecting_traffic | COUNTERFACTUAL | 1946.00 | 785.33 | 1350.00 | 15 | 0 | 0 | 24 | 21 |
| closure_during_execution | SCRIPTED | 800.33 | 64.33 | 768.00 | 9 | 0 | 3 | 6 | 3 |
| closure_during_execution | ADAPTIVE | 800.33 | 64.33 | 768.00 | 9 | 0 | 3 | 6 | 3 |
| closure_during_execution | COORDINATED_ADAPTIVE | 800.33 | 64.33 | 768.00 | 9 | 0 | 3 | 6 | 3 |
| closure_during_execution | ROLLING_HORIZON_COORDINATED | 800.33 | 64.33 | 768.00 | 9 | 0 | 3 | 6 | 3 |
| closure_during_execution | COUNTERFACTUAL | 799.00 | 63.00 | 768.00 | 9 | 0 | 0 | 6 | 3 |
| little_coordination_benefit | SCRIPTED | 1861.00 | 553.00 | 1861.00 | 15 | 0 | 6 | 0 | 0 |
| little_coordination_benefit | ADAPTIVE | 1861.00 | 553.00 | 1861.00 | 15 | 0 | 6 | 0 | 0 |
| little_coordination_benefit | COORDINATED_ADAPTIVE | 1861.00 | 553.00 | 1861.00 | 15 | 0 | 6 | 0 | 0 |
| little_coordination_benefit | ROLLING_HORIZON_COORDINATED | 1861.00 | 553.00 | 1861.00 | 15 | 0 | 6 | 0 | 0 |
| little_coordination_benefit | COUNTERFACTUAL | 1957.00 | 553.00 | 1957.00 | 15 | 0 | 6 | 0 | 0 |
| early_closure_merge | SCRIPTED | 768.00 | 32.00 | 768.00 | 9 | 0 | 0 | 3 | 0 |
| early_closure_merge | ADAPTIVE | 768.00 | 32.00 | 768.00 | 9 | 0 | 0 | 3 | 0 |
| early_closure_merge | COORDINATED_ADAPTIVE | 768.00 | 32.00 | 768.00 | 9 | 0 | 0 | 3 | 0 |
| early_closure_merge | ROLLING_HORIZON_COORDINATED | 768.00 | 32.00 | 768.00 | 9 | 0 | 0 | 3 | 0 |
| early_closure_merge | COUNTERFACTUAL | 768.00 | 32.00 | 768.00 | 9 | 0 | 0 | 3 | 0 |
| difficult_local_choice | SCRIPTED | 2243.00 | 1137.33 | 1295.00 | 15 | 0 | 0 | 15 | 12 |
| difficult_local_choice | ADAPTIVE | 2243.00 | 1137.33 | 1295.00 | 15 | 0 | 0 | 15 | 12 |
| difficult_local_choice | COORDINATED_ADAPTIVE | 2243.00 | 1137.33 | 1295.00 | 15 | 0 | 0 | 15 | 12 |
| difficult_local_choice | ROLLING_HORIZON_COORDINATED | 2243.00 | 1137.33 | 1295.00 | 15 | 0 | 0 | 15 | 12 |
| difficult_local_choice | COUNTERFACTUAL | 2243.00 | 785.33 | 1647.00 | 15 | 0 | 0 | 24 | 21 |

## Fleet comparison

Mean of per-scenario means (each scenario is weighted equally):

| Policy | Taxi s | Wait s | Distance m | Departure delay s |
|---|---:|---:|---:|---:|
| SCRIPTED | 1199.63 | 425.20 | 963.40 | 821.41 |
| ADAPTIVE | 1221.83 | 408.50 | 1002.60 | 828.91 |
| COORDINATED_ADAPTIVE | 1221.23 | 408.50 | 1002.60 | 828.91 |
| ROLLING_HORIZON_COORDINATED | 1221.23 | 408.50 | 1002.60 | 828.91 |
| COUNTERFACTUAL | 1279.90 | 292.57 | 1158.20 | 842.13 |

Differences are descriptive over three fixed seeds. Treat small or inconsistent changes as operationally inconclusive; the matrix does not establish statistical significance.

## Files

- `experiment.json` and `summary.json`: inputs, configuration, aggregate results, safety, and replay checks.
- `results.csv`: scenario by policy metrics.
- `summary.json`: per-aircraft taxi duration by flight.
- `config/scenarios/`: exact policy configured scenario inputs.
- `scenarios/`: original source definitions before the policy-only experiment edit.
- `runs/`: normalized metrics, ordered event records, and structured decisions for every serial run.
- `representative_decision_traces.json`: sampled decisions with all evaluated candidate rollout results.
- `SHA256SUMS.txt`: SHA-256 integrity ledger for all bundle files except the ledger itself.
- `counterfactual_vs_rolling_horizon`: matched-run comparison counts for different decisions and resulting outcomes.
- `timing`: mean complete simulator process runtime by policy; compare COUNTERFACTUAL with ROLLING_HORIZON_COORDINATED as an overhead proxy.
- `goal39-portable-evidence.zip`: ZIP archive in the parent evidence folder.
