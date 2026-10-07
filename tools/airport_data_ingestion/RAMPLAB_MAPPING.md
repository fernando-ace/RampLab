# Deferred RampLab mapping (Goal 24B)

No simulator integration is implemented in Goal 24A. Future scenario generation should consume the canonical package and make policy decisions explicitly:

| Canonical data | Future RampLab target | Mapping and open work |
|---|---|---|
| Airport/dataset | Scenario configuration | Keep IDs, name, timezone, dataset provenance as metadata; define airport geometry separately. |
| Flight | Aircraft mission and timing | Map operation and schedule to mission/arrival/departure timing; decide handling of actual vs estimated times and paired legs. |
| Aircraft | Aircraft entity/configuration | Map ID/type where supported; translate supplied dimensions to simulator units; performance/category fields need explicit internal support or remain metadata. |
| Gate | Gate/start/end assignment | Resolve gate identifiers to scenario gate resources; add geometry/occupancy policy separately. |
| Equipment | Vehicle fleet | Map supported categories to fleet types and initial locations; capability/capacity/assignment fields may need simulator extensions. |
| Turnaround requirement | Turnaround task | Map service type, duration, aircraft/flight and dependencies; determine task identity, priority, and missing internal service categories. |
| Equipment outage | Fleet outage event | Map equipment ID and interval; choose recovery/reassignment semantics in scenario generation. |
| Route closure | Closure/disruption event | Resource ID remains structurally validated only; resolve against geometry in Goal 24B and define rerouting policy. |
| Delayed service | Task disruption | Resolve explicit service/task identity and delay interval; source task IDs may not exist in current model. |
| Flight delay | Schedule adjustment | Apply delay or updated time once; define precedence between scheduled, estimated, actual, and delay amount. |
| Gate unavailability | Gate/resource disruption | Resolve gate resource and interval; define conflict behavior when an existing flight is assigned to the closed gate. |

Goal 24B should add a versioned generator and integration tests without changing the neutral ingestion contract. Unsupported simulator concepts should remain metadata or produce explicit generation errors, never be silently discarded.
