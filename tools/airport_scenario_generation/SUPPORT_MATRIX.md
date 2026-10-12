# Goal 24A feature support matrix

The machine-readable form is [support_matrix.json](support_matrix.json). Support is based on current native RampLab scenario fields and behavior, not on canonical schema availability.

| Canonical concept | Status | Behavior |
|---|---|---|
| Airport and dataset metadata | Metadata only | Preserved in generated manifest; geometry comes from the mapping resource. |
| Arrival/departure flights | Supported with transformation | Paired legs become one existing arrival-turnaround aircraft operation; canonical IDs remain traceable. |
| UTC schedule | Supported with transformation | Absolute timestamps map to deterministic integer simulation seconds. |
| Aircraft IDs/types/dimensions/registration | IDs supported; descriptive attributes metadata only | Existing aircraft behavior is used; no physical size/performance model is inferred. |
| Gates | Supported with transformation | Explicit canonical-to-existing gate mapping. |
| Fuel, baggage, catering, lavatory, potable-water, GPU, tug, stairs, air-start equipment | Supported with transformation when mapped | Each equipment mapping selects a distinct simulator service capability and spawn node; mobile resources use the existing routed fleet and safety system. |
| Service cart and generic ground service vehicle | Metadata only unless explicitly mapped | Unmapped equipment is retained as metadata and warned about; no capability is inferred from its label. |
| Fuel, baggage, catering, lavatory, potable-water, GPU, pushback, stairs, air-start tasks | Supported with transformation when mapped | Requirement minutes become task seconds and require a finite matching vehicle capability. A missing service or equipment mapping blocks generation. |
| Deboarding, cabin cleaning, maintenance inspection, chocks, pushback preparation | Supported with transformation when mapped | These use finite abstract crew capacity; no mobile GSE is fabricated. |
| Equipment outage | Supported with transformation only for an indefinite outage on mapped equipment | Uses native vehicle outage event; finite outage windows cannot restore a vehicle and are warned as unsupported. |
| Route closure | Supported with transformation when route maps | Uses native road edge close event; unresolved route produces a warning. |
| Delayed service | Supported with transformation when one generated task resolves | Uses the native task-duration disruption event. |
| Flight delay | Supported with transformation | Adds delay minutes to the targeted canonical leg before time conversion. |
| Gate unavailability | Unsupported | No safe existing gate-resource outage behavior. |
| Source provenance | Supported | Original Goal 24A source names and SHA-256 hashes are referenced from the package. |

Blocking unsupported features are required turnaround task types with no mapping, unresolved required flight/gate relationships, invalid canonical package status, invalid geometry references, and ambiguous multiple legs that the simulator cannot represent. Optional unsupported equipment/disruption concepts continue only with stable warnings in the manifest. Aircraft performance, dimensions, schedule priority, gate status, and gate capacity are not invented.
