# KAUO geometry readiness

**Decision: DATA_BLOCKED**

Profile: `production`; execution scope: `PRODUCTION`.

| Requirement | Status | Reason |
|---|---|---|
| REFERENCE_GEOMETRY | PASS | Airport reference point is supplied. |
| RUNWAY_GEOMETRY | PASS | Required runway axes and both threshold ends are present. |
| TAXIWAY_GEOMETRY | BLOCKED | No taxiway centerlines are supplied. |
| TAXIWAY_CONNECTIVITY | BLOCKED | Routing graph is absent or disconnected. |
| RUNWAY_CONNECTIONS | BLOCKED | No source-backed runway entry/exit relationship is supplied. |
| STAND_ACCESS | BLOCKED | Stand geometry or a source-backed route to a runway entry/exit is missing. |
| GSE_ACCESS | BLOCKED | GSE staging/depot access or a connected GSE route to a stand is missing. |
| SERVICE_ROADS | BLOCKED | Service-road geometry is missing. |
| PUSHBACK_TURNAROUND | BLOCKED | Stand pushback/turnaround connectivity is missing or unsupported. |
| RUNWAY_CROSSINGS | BLOCKED | No routing graph or runway-crossing permission geometry is supplied. |
| ROUTE_CLEARANCES | UNKNOWN | Required width/boundary or clearance evidence is insufficient. |
| SOURCE_AUTHORITY | PASS | Active feature authorities satisfy the production policy. |
| PROVENANCE_COMPLETENESS | PASS | Feature provenance and schema are complete. |
| GRAPH_VALIDITY | BLOCKED | Graph is missing or has blocking topology diagnostics. |
