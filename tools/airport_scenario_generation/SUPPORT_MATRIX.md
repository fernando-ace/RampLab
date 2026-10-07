# Goal 24A feature support matrix

The machine-readable form is [support_matrix.json](support_matrix.json). Support is based on current native RampLab scenario fields and behavior, not on canonical schema availability.

| Canonical concept | Status | Behavior |
|---|---|---|
| Airport and dataset metadata | Metadata only | Preserved in generated manifest; geometry comes from the mapping resource. |
| Arrival/departure flights | Supported with transformation | Paired legs become one existing arrival-turnaround aircraft operation; canonical IDs remain traceable. |
| UTC schedule | Supported with transformation | Absolute timestamps map to deterministic integer simulation seconds. |
| Aircraft IDs/types/dimensions/registration | IDs supported; descriptive attributes metadata only | Existing aircraft behavior is used; no physical size/performance model is inferred. |
| Gates | Supported with transformation | Explicit canonical-to-existing gate mapping. |
| Fuel/baggage equipment | Supported with transformation | Maps to existing fueling/baggage dispatch capabilities and geometry spawn nodes. |
| Catering equipment | Supported with transformation, using generic behavior | The generated vehicle retains its catering label, but current dispatch treats non-fueling vehicles through generic baggage-delivery behavior; there is no distinct catering capability. |
| Tug, service cart, generic ground service vehicle | Metadata only | No validated matching fleet behavior exists. |
| Fuel, baggage, catering, deboarding, cleaning, baggage load, pushback service tasks | Supported with transformation when mapped | Requirement minutes become task seconds; fueling uses fueling dispatch, and other mobile tasks currently use generic baggage-delivery dispatch. A missing required service mapping blocks generation. |
| Equipment outage | Supported with transformation only for an indefinite outage on mapped equipment | Uses native vehicle outage event; finite outage windows cannot restore a vehicle and are warned as unsupported. |
| Route closure | Supported with transformation when route maps | Uses native road edge close event; unresolved route produces a warning. |
| Delayed service | Supported with transformation when one generated task resolves | Uses the native task-duration disruption event. |
| Flight delay | Supported with transformation | Adds delay minutes to the targeted canonical leg before time conversion. |
| Gate unavailability | Unsupported | No safe existing gate-resource outage behavior. |
| Source provenance | Supported | Original Goal 24A source names and SHA-256 hashes are referenced from the package. |

Blocking unsupported features are required turnaround task types with no mapping, unresolved required flight/gate relationships, invalid canonical package status, invalid geometry references, and ambiguous multiple legs that the simulator cannot represent. Optional unsupported equipment/disruption concepts continue only with stable warnings in the manifest. Aircraft performance, dimensions, schedule priority, gate status, and gate capacity are not invented.
