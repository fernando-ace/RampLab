# KAUO geometry readiness

**Decision: DATA_BLOCKED**

Profile: `production`; execution scope: `PRODUCTION`.

| Requirement | Status | Reason |
|---|---|---|
| REFERENCE_GEOMETRY | PASS | Airport reference point is supplied. |
| RUNWAY_GEOMETRY | BLOCKED | A required runway centerline, identifier, or matching pair of threshold ends is missing. |
| TAXIWAY_GEOMETRY | BLOCKED | Candidate centerlines exist but none meets production authority/confidence approval. |
| TAXIWAY_CONNECTIVITY | BLOCKED | Routing graph is absent or disconnected. |
| RUNWAY_CONNECTIONS | BLOCKED | No source-backed runway entry/exit relationship is supplied. |
| STAND_ACCESS | BLOCKED | Stand geometry or a source-backed route to a runway entry/exit is missing. |
| GSE_ACCESS | BLOCKED | GSE staging/depot access or a connected GSE route to a stand is missing. |
| SERVICE_ROADS | BLOCKED | Service-road geometry is missing. |
| PUSHBACK_TURNAROUND | BLOCKED | Stand pushback/turnaround connectivity is missing or unsupported. |
| RUNWAY_CROSSINGS | BLOCKED | A runway intersection lacks explicit source-backed permission geometry. |
| ROUTE_CLEARANCES | UNKNOWN | Required width/boundary or clearance evidence is insufficient. |
| SOURCE_AUTHORITY | BLOCKED | One or more active features are not approved under the production authority policy. |
| PROVENANCE_COMPLETENESS | PASS | Feature provenance and schema are complete. |
| GRAPH_VALIDITY | BLOCKED | Graph is missing or has blocking topology diagnostics. |
