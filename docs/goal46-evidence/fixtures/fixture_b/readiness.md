# SYNTHETIC-B geometry readiness

**Decision: DATA_BLOCKED**

Profile: `test_fixture`; execution scope: `TEST_FIXTURE_ONLY`.

| Requirement | Status | Reason |
|---|---|---|
| REFERENCE_GEOMETRY | PASS | Airport reference point is supplied. |
| RUNWAY_GEOMETRY | PASS | Required runway axes and both threshold ends are present. |
| TAXIWAY_GEOMETRY | PASS | Taxiway centerline geometry is present. |
| TAXIWAY_CONNECTIVITY | BLOCKED | Routing graph is absent or disconnected. |
| RUNWAY_CONNECTIONS | PASS | Source-backed runway entry/exit relationships are present. |
| STAND_ACCESS | BLOCKED | Stand geometry or a source-backed route to a runway entry/exit is missing. |
| GSE_ACCESS | PASS | All GSE depots connect to the stand/service network. |
| SERVICE_ROADS | PASS | Service-road geometry is supplied. |
| PUSHBACK_TURNAROUND | BLOCKED | Stand pushback/turnaround connectivity is missing or unsupported. |
| RUNWAY_CROSSINGS | PASS | All routed runway crossings have explicit permission evidence. |
| ROUTE_CLEARANCES | PASS | All required clearance assessments pass and every route edge has source-backed width. |
| SOURCE_AUTHORITY | PASS | Active feature authorities satisfy the test_fixture policy. |
| PROVENANCE_COMPLETENESS | PASS | Feature provenance and schema are complete. |
| GRAPH_VALIDITY | BLOCKED | Graph is missing or has blocking topology diagnostics. |

## Stand to runway routes

- **stand_A1 — FAIL**: stand_A1: source-backed arrival exit to stand or stand to runway entry route is missing. Path: `no path`.

## GSE to stand routes

- **gse_depot_1 — PASS**: Source-backed GSE route exists. Path: `path n0004 → n0003`.

## Turnaround routes

- **stand_A1 — FAIL**: stand_A1: turnaround or pushback connectivity is not source-backed. Path: `no path`.
