# SYNTHETIC-A geometry readiness

**Decision: SIMULATOR_READY**

Profile: `test_fixture`; execution scope: `TEST_FIXTURE_ONLY`.

| Requirement | Status | Reason |
|---|---|---|
| REFERENCE_GEOMETRY | PASS | Airport reference point is supplied. |
| RUNWAY_GEOMETRY | PASS | Required runway axes and both threshold ends are present. |
| TAXIWAY_GEOMETRY | PASS | Taxiway centerline geometry is present. |
| TAXIWAY_CONNECTIVITY | PASS | Routing graph is connected. |
| RUNWAY_CONNECTIONS | PASS | Source-backed runway entry/exit relationships are present. |
| STAND_ACCESS | PASS | All stands have source-backed routes to runway entry/exit nodes. |
| GSE_ACCESS | PASS | All GSE depots connect to the stand/service network. |
| SERVICE_ROADS | PASS | Service-road geometry is supplied. |
| PUSHBACK_TURNAROUND | PASS | Every stand has a source-backed turnaround/pushback link. |
| RUNWAY_CROSSINGS | PASS | All routed runway crossings have explicit permission evidence. |
| ROUTE_CLEARANCES | PASS | All required clearance assessments pass and every route edge has source-backed width. |
| SOURCE_AUTHORITY | PASS | Active feature authorities satisfy the test_fixture policy. |
| PROVENANCE_COMPLETENESS | PASS | Feature provenance and schema are complete. |
| GRAPH_VALIDITY | PASS | Graph has source lineage and no blocking topology diagnostics. |

## Stand to runway routes

- **stand_A1 — PASS**: Arrival and departure routes exist. Path: `arrival n0001 → n0002; departure n0002 → n0001`.

## GSE to stand routes

- **gse_depot_1 — PASS**: Source-backed GSE route exists. Path: `path n0003 → n0002`.

## Turnaround routes

- **stand_A1 — PASS**: Source-backed turnaround/pushback access exists. Path: `path n0002`.
