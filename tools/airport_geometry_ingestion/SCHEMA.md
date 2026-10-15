# Airport operational geometry schema 2.1.0

The Goal47 package extends the Goal46 airport ingestion and geospatial code. It does not change the Goal24 tabular package or the Goal45 KAUO source dataset. The normalized artifact is deterministic UTF-8 JSON with sorted object keys and a final newline. It emits no generation timestamp.

## Top-level model

| Field | Meaning |
|---|---|
| `schema_version` | Exact semantic schema version, currently `2.1.0`. Unknown versions fail validation. |
| `dataset_id` | Stable caller-provided revision identifier. |
| `airport` | Identifier, optional name/IATA/ICAO, source and normalized CRS, units, airport reference point, and optional elevation/datum. |
| `coordinate_system` | Source CRS, normalized CRS (`LOCAL_ENU`), metres, axis names, transform description and parameters, and tolerance. |
| `sources` | Source register. Each source records its ID/type, original filename, optional URL/publication date, SHA-256, extraction method, authority, and notes. |
| `features` | Active normalized airport geometry. Every item has a stable ID, feature type, GeoJSON geometry type, ENU coordinates, source properties, and provenance. |
| `quarantined_features` | Explicitly retained features that cannot participate in active routing, such as Goal45’s approximate legacy KAUO graph. |
| `operational_graph` | Deterministic routing nodes and directed/undirected edges, with permissions, widths, geometry, source feature lineage, and construction tolerance. |
| `operational_relationships` | Source-backed relationships such as runway entry/exit, stand access, turnaround, crossing, and service access. Every relationship cites a registered source and active source features. |
| `clearance_assessments` | Measured clearance records with required/measured metres, assessment type/method, source ID, and assessed feature IDs. |
| `authority_policy` | Machine-readable production, visualization, testing, and safety-claim rules. |
| `simulation_assumptions` | Explicit simulator-only operational policies, separate from geometry and airport facts. |

Feature types include airport reference point, runway axis and runway pavement polygon, runway threshold, taxiway centerline/polygon/intersection, hold-short position, apron, aircraft stand, service road, GSE staging/access/depot, runway crossing, restricted/protected area, routing node/edge, and other surface polygon. Geometry must agree with the declared feature type. Polygon coordinates preserve all rings. Widths, clearances, permissions, runway IDs, stand IDs, vehicle classes, access-node IDs, and other supplied attributes remain in `properties`; the importer does not rename similar fields by guessing.

## Provenance and authority

Every active or quarantined feature records `source_id`, `original_feature_id`, `source_file`, `source_sha256`, optional `source_url` and `publication_date`, `extraction_method`, `transformation`, `authority`, `geometry_origin`, `accuracy_m`, and an explicit Boolean `simulation_approved`. Derived and inferred geometry additionally requires `derived_from` feature IDs. Graph edges carry the input feature ID and its provenance. Graph nodes list all input features, source IDs, source hashes, transformation, authority, and approval state.

Authority values are `AUTHORITATIVE`, `SOURCE_DERIVED`, `OPERATOR_PROVIDED`, `SURVEYED`, `APPROXIMATE`, `SYNTHETIC`, and `UNKNOWN`. `SOURCE_DERIVED` features require confidence `HIGH`, `MEDIUM`, or `LOW` and a reproducible derivation record. Production approval requires HIGH confidence, known horizontal accuracy no worse than 1 m, a derivation configuration SHA-256, an independent corroborating production-grade source, and passing CRS, airport-bounds, geometry, and independent-review checks. MEDIUM and LOW remain context only. Production routing requires complete lineage and explicit approval, and rejects inferred geometry. `APPROXIMATE`, `SYNTHETIC`, and `UNKNOWN` geometry cannot be simulation-approved. Visualization can display every class with a distinct style. Synthetic data is accepted only under `test_fixture` readiness scope. A test fixture can exercise a full readiness pass, but its report always sets `production_simulator_allowed: false`.

The source registry authority and feature authority must be compatible. A registered authoritative, surveyed, or operator-provided source may produce source-derived features. A weaker source cannot label its feature authoritative. Positional accuracy and source-attribution confidence are separate fields; confidence never substitutes for accuracy. The legacy numeric `confidence` field does not establish a HIGH confidence level or grant routing eligibility.

## Coordinates and transforms

All normalized coordinates use local ENU metres: `+X east`, `+Y north`, and optional `+Z up`. Supported input coordinate systems are:

- `EPSG:4326`, `EPSG:4979`, `OGC:CRS84`, or `WGS84`. GeoJSON/KML axis order is explicitly longitude, latitude, optional altitude. A mapping must provide the geodetic origin (latitude, longitude, ellipsoid height). Conversion reuses RampLab’s WGS-84 ECEF to tangent-plane ENU transform.
- `LOCAL_ENU` / `RAMPLAB_LOCAL_ENU`, with explicit `m` or `ft` units and east/north axes.
- `LOCAL_SIMULATOR` / `LOCAL_GRID`, with explicit `m` or `ft`, a positive-X true bearing, and optional ENU origin offsets. The transform reuses RampLab’s local-grid rotation helper. Axis orientation is never inferred.

GeoJSON 3D height follows its declared WGS-84 coordinate convention. KML altitude values are retained as source metadata and excluded from normalized Z because an altitude mode/vertical datum cannot be assumed. MSL-to-ellipsoid conversion is not performed without an explicit geoid model. Projected CRS such as UTM, CAD unit inference, and vertical datum conversion are not currently supported; they fail with an actionable message rather than being silently reinterpreted. Coordinate tolerances and transform parameters are recorded in the canonical dataset.

## Import formats and mappings

Implemented adapters: GeoJSON FeatureCollection, explicit CSV point/edge rows, KML, KMZ, canonical RampLab schema 2.0 JSON, and migration of the existing Goal45 schema 1.0 KAUO geometry file. GeoJSON, CSV, KML, and KMZ require an explicit mapping file naming identifier and feature-type fields. CSV requires either a JSON coordinate-array field or explicit point/edge coordinate columns. Mapped properties retain source names and values. See [`MAPPING_EXAMPLES.md`](MAPPING_EXAMPLES.md).

Shapefile, GeoPackage, DXF, WKT, and WKB are not implemented. This host has no geospatial library installed; the current formats cover multiple common exchange paths without adding a runtime dependency or creating an untested parser. These are explicit unsupported formats, not silently dropped data.

## Graph and topology rules

The builder creates graph edges only from supplied taxiway, service-road, GSE-access-link, or routing-edge line geometry. A `LineString` or each `MultiLineString` part preserves its complete coordinate path and length. Endpoints may be snapped together only when every pair in the endpoint cluster is within the explicit tolerance (default 0.25 m). Snapping creates a derived node with all contributing feature IDs and hashes; it never creates an edge. The builder does not extend lines, connect a nearby stand automatically, infer permissions, assume bidirectional movement, or bridge a gap.

Directionality and vehicle permissions are required for a graph edge to support a route. Missing fields remain unknown and create diagnostics. Supplied line intersections away from shared endpoints are reported as ambiguous and are not auto-connected; a runway intersection additionally requires an explicit crossing feature and a source-backed permission on the route. Diagnostics cover disconnected components, missing endpoints, zero-length edges/segments, duplicate geometry/edges, self-intersections, ambiguous intersections, suspicious near misses, and unused graph nodes. Dead-end termini and close-but-unsnapped endpoints are warnings; they remain visible in the report.

Topology reporting includes runway exit-to-stand arrival paths, stand-to-runway-entry departure paths, stand turnaround/pushback links, and GSE depot-to-stand routes. Reports include node paths and understandable failure text. Unreachable routes are not represented as successful by a Boolean default.

## Clearances and readiness

Clearance assessments resolve to `PASS`, `FAIL`, or `UNKNOWN`. A PASS requires finite measured and required metres, source and method, valid feature lineage, explicit production approval (unless running the isolated test-fixture profile), and known positive width on every graph route edge. Missing measurements, source approvals, features, or widths produce `UNKNOWN`; they never become safe-by-default.

Each readiness requirement is `PASS`, `FAIL`, `BLOCKED`, `UNKNOWN`, or `NOT_APPLICABLE` and includes reason, missing information, source IDs, supporting feature IDs, and diagnostics/paths where applicable. The final decision is `SIMULATOR_READY` only when every mandatory production requirement passes or is explicitly not applicable; otherwise it is `DATA_BLOCKED`. Only a production-profile `SIMULATOR_READY` report can set `production_simulator_allowed: true`. The current code reports readiness and does not launch the simulator.

Requirements cover reference and runway geometry (including paired runway ends), taxiways and routing connectivity, runway entry/exit, stand access, turnaround/pushback, GSE/service-road access, crossings, route clearances, source authority, provenance, and graph validity.
