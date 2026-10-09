# KAUO support and accuracy matrix

| Category | Modeled content | Evidence and limit |
|---|---|---|
| Real / authoritative | KAUO identity; airport reference coordinates; 777 ft field elevation; runway 18/36 (5,264 × 100 ft asphalt-grooved); runway 11/29 (4,000 × 75 ft asphalt); charted relative placement of taxiway A, terminal/FBO apron, west apron, hangar areas and self-serve fuel farm | FAA chart and Chart Supplement cited in `tools/airport_data_ingestion/examples/kauo/PROVENANCE.md`. |
| Real / public observation | Auburn University airport states full-service FBO, fuels, and flight-training operators | Official airport website; categories only, no individual flight or equipment observation. |
| Derived | Decimal coordinates from degree/minute notation; runway dimensions in meters; chart-graticule feature offsets transformed through the existing Cesium heading and origin offset | Unit conversions and coordinate transforms are calculated; chart points remain approximate. |
| Approximate | Chart-digitized runway midpoints, taxiway/apron graph anchors, edge distances, detour, fuel-farm and hangar anchors, parking point | Diagram-informed points; not a centerline survey, pavement polygon, or published stand coordinate. |
| Assumed | One representative GA arrival/departure pair, aircraft archetype, operation timestamps, abstract pushback-preparation task and its two-minute duration, taxi speed 5 m/s, rollout/runway occupancy timings | Scenario-exercise inputs. They are neither actual KAUO movements nor airport performance measurements. |
| Unsupported | Event-level flight history, actual runway choice, real taxi track/time, real parking assignment, aircraft registration/model, collision-suitable aircraft envelope, equipment inventory/spawn, service duration/distribution, live NOTAM/closure, surveyed road/pavement geometry | Not obtained from public sources for this prototype. |
| Not applicable to the sample | Airline gates, passenger terminal bridges, baggage handling, catering, cabin cleaning, commercial airline turnaround schedule | Not inferred from the airport's FBO services or the simulator's available task types. |

The result supports only a **KAUO-calibrated simulation prototype** based on limited public static data. FBO fuel availability is public, but simulated fueling is unsupported because equipment inventory, dispatch locations, and service durations are unknown. The abstract pushback-preparation task is a scenario input, not an observed ground operation. It does not support an operational digital-twin or safety-validation claim.

The generated runs have one aircraft and no ground vehicles. Their zero collision counts are observed for those modeled actors, but aircraft-aircraft, aircraft-ground, and fleet minimum-separation fields have no actor pair to measure; exported `0.0` separation values must be reported as **not applicable**, not as zero-meter clearance.
