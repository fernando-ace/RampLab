# Mapping configuration

The mapping is a versioned JSON file stored beside its geometry document. `mapping.synthetic.json` maps Goal 24A's synthetic IDs to the existing `mixed_runway_operations` geometry copied into `simulator_geometry.json`. Geometry, identifiers, and simulator classes are separate from the generator logic so reviewers can inspect and replace the policy.

| Field | Meaning |
|---|---|
| `geometry_file` | Relative simulator geometry JSON. It must stay inside the mapping directory and define nodes, edges, gates, and surface operations. |
| `gate_ids` | Canonical gate ID to existing RampLab gate ID. Every canonical gate must be resolved, including unused gates. Duplicate/unknown targets fail. |
| `route_ids` | Canonical route/resource ID to existing graph edge ID. This is required to apply a route closure. |
| `equipment_types` | Canonical equipment type to supported RampLab vehicle/service class and deterministic initial node. An equipment record may override the node with `initial_location` only when that value is a simulator node ID. |
| `service_types` | Canonical required service type to an existing RampLab turnaround task type. Unmapped required tasks fail. |
| `arrival_exit_node`, `departure_handoff_node`, `runway_node` | Existing simulator geometry node IDs used by the surface-operation model. |
| `seed` | Default deterministic simulation seed; `generate --seed` overrides it. |

Generation groups one arrival and at most one departure leg for each canonical aircraft. The pair must resolve to the same gate. Both flight IDs map to the resulting RampLab aircraft entity in `identity-map.json`. More than one arrival or departure leg for an aircraft is rejected because the existing scenario model has no multi-leg rotation representation.

Flight `actual_time` takes precedence over `estimated_time`, which takes precedence over `scheduled_time`. Canonical `flight_delay` records then add their delay minutes to the targeted leg. Times are UTC instants and round to nearest integer second. Turnaround requirement durations convert minutes to seconds. One task of each RampLab service class per aircraft is supported by the current native loader; duplicate mapped task classes fail with the requirement IDs in the error.

The existing simulator requires at least one fueling and one baggage vehicle, even if no requirement uses one of those services. The generator rejects a mapped equipment location not present in geometry and warns when canonical equipment has no safe simulator class. A tug is metadata only: it is never relabeled as a different vehicle class.

Vehicle spawn nodes must be distinct and clear of active aircraft arrival/runway nodes. The synthetic mapping places `FUEL-1` at `depot`, `BAG-1` at `south`, and the unused `CAT-1` at `gate_a3`. This avoids starting two autonomy vehicles at the same coordinates and keeps the baggage vehicle clear of the runway arrival point while preserving a directed route to the service gate.

Route closures map to existing edge disable events. If a canonical closure includes an end time, the generator pairs the close and reopen events for the same edge. Equipment outages are only mapped when indefinite: the current vehicle-outage event has no restore behavior, so finite outage windows are explicitly warned and omitted. Gate unavailability is unsupported because the current simulator has no validated gate outage behavior.
