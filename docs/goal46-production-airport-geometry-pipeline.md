# Goal46 production airport geometry acquisition and import pipeline

## Goal and safety boundary

Goal45 established that public KAUO references support the airport reference point, four runway thresholds, and two source-derived centerlines, but not the current taxiway graph, stand/service access, runway crossing permissions, or operational clearances. Goal46 makes that missing operational data ingestible and auditable when it is supplied. It does not manufacture those features, weaken Goal45’s gate, or run the simulator on incomplete KAUO data.

## Architecture

The new package is `tools/airport_geometry_ingestion/`. It consolidates spatial normalization around the Goal45 source-backed feature model and reuses `tools/airport_scenario_generation/geospatial.py` for WGS-84 ECEF-to-ENU conversion and explicit local-grid rotation. The existing Goal24A/24B tabular ingestion remains the source for flights, aircraft, equipment, and scenario generation; Goal46 does not create a competing tabular schema.

The data path is:

1. Read a raw source and explicit mapping (or a schema 2.0 canonical file / Goal45 migration).
2. Hash and register source bytes; normalize only supported CRS/units into local ENU metres.
3. Validate schema, feature geometry, source/authority compatibility, provenance, and lineage.
4. Construct routing edges from supplied linework. Snap only supplied endpoints within the declared tolerance; retain complete polylines and source lineage.
5. Audit graph diagnostics, runway/stand/GSE routes, crossings, source authority, and clearance evidence.
6. Emit deterministic JSON, Markdown, SVG, comparison reports, fixture artifacts, checksum ledger, and ZIP.

## Schema and import formats

The canonical model is version `2.0.0`. It represents airport metadata, CRS/units/transform, runways and thresholds, taxiway lines/polygons/intersections/hold-short locations, aprons, stands, GSE/service infrastructure, routing graph, restricted/protected geometry, source registry, feature provenance, operational relationships, and clearance assessments. Active and quarantined geometry are separate top-level collections. Graph nodes and edges retain their contributing feature IDs and source hashes.

Implemented adapters are GeoJSON FeatureCollection, explicit CSV point/edge rows, KML, KMZ, canonical schema 2.0 JSON, and migration of Goal45 schema 1.0 KAUO JSON. Field and source mappings are explicit; ambiguous columns are rejected. Shapefile, GeoPackage, DXF, WKT/WKB, projected CRS, and unknown vertical-datum transforms are not implemented. They fail with a message and can be converted by an external documented workflow before import. No geospatial dependency is added to RampLab.

## Coordinates

Canonical coordinates are metres in local ENU (`+X east`, `+Y north`, optional `+Z up`). WGS-84 geographic coordinates transform using the existing project geospatial implementation and an explicit airport origin. EPSG:4326 and GeoJSON/KML follow longitude, latitude order. Local ENU supports explicit metres or feet. Local simulator grids require positive-X true bearing and optional ENU offsets; axis direction is never assumed. KML altitude is preserved in source properties but excluded from canonical vertical geometry because its altitude mode/datum is not established. Transform parameters and tolerances are machine-readable. Known Goal45 FAA threshold coordinates and deterministic transforms are regression-tested.

## Authority and provenance

The policy distinguishes `AUTHORITATIVE`, `SOURCE_DERIVED`, `OPERATOR_PROVIDED`, `SURVEYED`, `APPROXIMATE`, `SYNTHETIC`, and `UNKNOWN`. Production routing accepts only the first four, and derived geometry must identify its source features. Approximate, synthetic, unknown, inferred, or unapproved features cannot enter production routing. Visualization can show all classes, including quarantine. Test fixtures use `profile: test_fixture`; their readiness result is explicitly `TEST_FIXTURE_ONLY`, and `production_simulator_allowed` remains false even when the synthetic complete fixture passes.

Each source includes source ID/type/file/hash, optional URL/publication date, extraction method, authority, and notes. Each feature points to its registered source and retains original feature ID, source hash, authority, transformation, accuracy, optional confidence, and direct/derived/inferred origin. Missing positional accuracy stays null; confidence is not treated as positional accuracy.

## Graph construction and topology

The graph builder reads taxiway, service-road, GSE-access, and routing-edge linework only. It does not restore the Goal42–44 approximate routes or synthesize missing airport infrastructure. It creates one edge per supplied LineString or MultiLineString part and records path geometry and length. Endpoint snapping is deterministic, pairwise within the caller’s metre tolerance, and adds no connecting segment. Directionality and vehicle permissions must be explicit. Missing direction/permissions prevent a route from being assumed.

The audit detects disconnected components, dangling/isolated nodes, duplicate edges/geometry, zero-length edges/segments, self-intersections, unsplit/ambiguous line intersections, and suspicious near misses. Warnings remain visible; blocking diagnostics keep the graph from passing. It reports arrival runway-exit-to-stand and departure stand-to-runway-entry paths, turnaround/pushback relations, and GSE depot-to-stand paths. A line intersection without a source-backed explicit connection is not automatically a graph node. Routes crossing runway linework require both explicit crossing geometry and source-backed permission metadata.

## Clearance and readiness gate

Clearance states are `PASS`, `FAIL`, or `UNKNOWN`. A passing audit needs finite nonnegative values, a registered source and method, feature references, explicit approval under the selected profile, and positive known widths on all graph edges. Missing measurement/width/boundary evidence stays `UNKNOWN` and blocks production readiness.

Readiness emits per-requirement status, supporting feature IDs, source IDs, reason, missing information, topology paths, and diagnostics. The mandatory checks include reference/runway geometry and both thresholds, taxiway geometry/connectivity, runway entry/exit, stand access, turnaround/pushback, GSE access, service roads, runway crossings, route clearances, source authority, provenance, and graph validity. The decision is `SIMULATOR_READY` or `DATA_BLOCKED`. The readiness command does not launch the simulator.

## KAUO import result

The Goal45 dataset is migrated into schema 2.0 with **7 active features retained**: one airport reference point, four runway thresholds, and two runway centerlines. The existing **148 approximate legacy nodes/edges remain in `quarantined_features`**, each with provenance and not approved for simulation. They are excluded from active feature counts, graph construction, and production readiness. The migrated dataset validates, but readiness is **DATA_BLOCKED**.

The KAUO gate continues to block on:

- taxiway/taxilane centerlines, intersections and a source-backed routing graph;
- runway entry/exit links and route clearances at runway handoff points;
- current stand IDs/positions and aircraft access;
- stand turnaround/pushback connectivity;
- GSE staging/depot positions and access links;
- service roads and vehicle permissions;
- any actual runway crossings plus source-backed crossing permissions;
- source-backed widths/boundaries and measured aircraft/GSE route clearance assessments.

The source request draft is at [`goal46-kauo-data-request-draft.md`](goal46-kauo-data-request-draft.md); it has not been sent. Exact layer, CRS, provenance, and accuracy needs are in [`goal46-airport-data-requirements.md`](goal46-airport-data-requirements.md).

## Synthetic fixtures

Eight isolated synthetic-only datasets cover the full path: A complete readiness pass; B missing taxiway connection; C disconnected GSE network; D runway crossing without permission; E insufficient clearance data; F duplicate/conflicting geometry; G approximate geometry attempting production routing; H a moved/permission-changed revision. Fixture A passes only under the test-fixture profile. All other fixtures block for their intended issue. Their paths and outputs are saved under `docs/goal46-evidence/fixtures/`.

## Inspection, compare, and evidence

The deterministic SVG renders runways, polygons, taxiways, stands, roads/GSE features, graph edges/nodes, approximate quarantine, and failed requirement locations with authority-specific styles. Revision comparison reports added/removed features, measured movement, changed properties/permissions/provenance, connectivity changes, and readiness changes under an explicit profile. The evidence bundle includes a SHA-256 ledger, deterministic ZIP timestamps, per-file checksum verification, ZIP CRC verification, and a ZIP SHA-256 sidecar.

## Limitations

No actual operator geometry was received, no external data request was sent, and no real KAUO taxiway, stand, service road, GSE path, crossing, or clearance was invented. Projected CRS transforms, Shapefile/GeoPackage/DXF parsing, split-at-intersection construction, and automatic stand projection/attachment are not implemented. Intersections are diagnosed rather than connected when the source does not establish a safe relationship. The SVG is a review artifact, not a GIS application or accuracy claim. Synthetic readiness is not an airport safety claim.

## Validation

Exact Goal46 unit/regression results, Goal42–45 checks, Release build/CTest, repeated-output hashes, generated evidence, ZIP integrity, and simulator decision are recorded in `docs/goal46-evidence/validation-results.json` and its command logs.
