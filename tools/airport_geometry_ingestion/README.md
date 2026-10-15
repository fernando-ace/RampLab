# Airport operational geometry ingestion (Goals 46-47)

This package imports documented airport geometry into canonical schema 2.1.0, records source hashes and authority, builds a graph only from supplied linework, audits operational routes and clearances, reports readiness, compares revisions, and creates deterministic evidence archives. Goal47 adds a reproducible ALDOT pavement inventory import, USGS NAIP imagery source manifest, explicit source-derived confidence requirements, and a KAUO dataset build. It is separate from the Goal24 flight/turnaround tables and reuses Goal45’s KAUO source data and WGS-84/ENU transform.

Run the deterministic Goal47 data assembly with:

```powershell
python -m tools.airport_geometry_ingestion.goal47
```

This writes canonical data, geometry counts, readiness reports, and SVG comparison maps under `docs/goal47-evidence/`. The dataset remains `DATA_BLOCKED`: the ALDOT input provides pavement-section polygons, not taxiway centerlines, directed routes, stand access, GSE paths, or permissions. The imagery is retained for visual comparison but does not establish sub-meter positional accuracy.

## CLI

```powershell
# See available commands and flags
python -m tools.airport_geometry_ingestion --help

# Inspect a raw source using explicit field/coordinate mappings
python -m tools.airport_geometry_ingestion inspect input.geojson --mapping mapping.json

# Normalize GeoJSON, CSV, KML, or KMZ
python -m tools.airport_geometry_ingestion normalize input.geojson --mapping mapping.json --output results/airport.json

# Migrate the Goal45 KAUO dataset into schema 2.1 without promoting quarantine
python -m tools.airport_geometry_ingestion normalize tools/airport_data_ingestion/kauo_operational_geometry.json --output results/kauo_goal46.json

# Validate, construct graph, audit, and gate readiness
python -m tools.airport_geometry_ingestion validate results/airport.json --report results/validation.json
python -m tools.airport_geometry_ingestion build-graph results/airport.json --snap-tolerance-m 0.25 --output results/airport-graph.json
python -m tools.airport_geometry_ingestion audit results/airport-graph.json --json results/readiness.json --markdown results/readiness.md --svg results/inspection.svg
python -m tools.airport_geometry_ingestion readiness results/airport-graph.json --json results/readiness.json --profile production --require-ready

# Compare two schema 2 datasets and package evidence
python -m tools.airport_geometry_ingestion compare old.json new.json --json results/diff.json --markdown results/diff.md
python -m tools.airport_geometry_ingestion bundle docs/goal46-evidence --output docs/goal46-evidence.zip
```

Expected validation/readiness failures print concise messages and use exit code 2 where a command is expected to gate execution. `readiness` returns nonzero for a `DATA_BLOCKED` result only with `--require-ready`; the report always makes the decision explicit. The tool never invokes the simulator.

## Formats and limits

The CLI supports GeoJSON, explicit CSV point/edge records, KML, KMZ, canonical schema 2.1 JSON, and a dedicated Goal45 schema 1.0 KAUO migration. Every raw spatial format needs an explicit mapping; no field names, coordinate units, CRS, direction, vehicle permission, or source authority are inferred. KML altitude is retained but excluded from normalized Z when vertical datum/mode is unknown. Projected EPSG CRSs, shapefile, GeoPackage, DXF, WKT and WKB currently fail explicitly; they are documented as conversion inputs, not silently accepted.

Read [SCHEMA.md](SCHEMA.md) for canonical fields, authority policy, coordinate rules, graph construction and readiness semantics. Read [MAPPING_EXAMPLES.md](MAPPING_EXAMPLES.md) for GeoJSON, CSV, and KML mapping files.

## Tests

```powershell
python -m unittest tools.airport_geometry_ingestion.test_geometry -v
```

Fixture generator `write_fixtures()` creates eight per-fixture folders. Every source packet and canonical file carries an explicit synthetic label. Fixture A passes the test-only readiness profile; its report cannot authorize a production simulator run. The Goal45 KAUO migration is independently tested to retain seven active source-backed features and 148 quarantined approximate features.
