# SYNTHETIC-F geometry readiness

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
| GSE_ACCESS | BLOCKED | GSE staging/depot access or a connected GSE route to a stand is missing. |
| SERVICE_ROADS | PASS | Service-road geometry is supplied. |
| PUSHBACK_TURNAROUND | PASS | Every stand has a source-backed turnaround/pushback link. |
| RUNWAY_CROSSINGS | PASS | All routed runway crossings have explicit permission evidence. |
| ROUTE_CLEARANCES | UNKNOWN | Required width/boundary or clearance evidence is insufficient. |
| SOURCE_AUTHORITY | PASS | Active feature authorities satisfy the test_fixture policy. |
| PROVENANCE_COMPLETENESS | FAIL | Canonical schema/provenance validation found errors. |
| GRAPH_VALIDITY | BLOCKED | Graph is missing or has blocking topology diagnostics. |

## Stand to runway routes

- **stand_A1 — FAIL**: stand_A1: source-backed arrival exit to stand or stand to runway entry route is missing. Path: `no path`.

## GSE to stand routes

- **gse_depot_1 — FAIL**: gse_depot_1: depot is disconnected from the service-road/apron network or stand access. Path: `no path`.

## Turnaround routes

- **stand_A1 — PASS**: Source-backed turnaround/pushback access exists. Path: `path n0002`.
