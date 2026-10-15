# Goal46 evidence bundle

This bundle is reproducible evidence for the airport operational-geometry ingestion, normalization, topology audit, and readiness pipeline. The fixture datasets are synthetic test inputs and do not describe real airports. No production simulator run, external data request, upload, push, publication, or deployment was performed.

## Decision

**KAUO remains DATA_BLOCKED.** The converted Goal45 dataset passes the Goal46 schema checks and preserves seven source-backed reference/runway features. The 148 Goal42-44 approximate legacy route features are quarantined and excluded from the operational graph. The missing source data includes taxiway/taxilane centerlines and intersections, runway entry/exit relationships, stand access, turnaround/pushback, GSE staging routes, service roads and permissions, runway crossing geometry/permission, and width/boundary/clearance evidence. This evidence does not authorize KAUO simulation.

## Fixture matrix

Each `fixtures/fixture_X/` directory contains canonical schema JSON, its source packet, a readiness report, validation report, and SVG. All are labeled synthetic.

| Fixture | Expected result |
|---|---|
| A | Test-only `SIMULATOR_READY`; production permission remains false |
| B | `DATA_BLOCKED`: disconnected taxi route |
| C | `DATA_BLOCKED`: disconnected GSE access |
| D | `DATA_BLOCKED`: crossing permission missing/denied |
| E | Clearance `UNKNOWN`; `DATA_BLOCKED` |
| F | Conflicting duplicate geometry; validation fails and readiness blocks |
| G | Approximate geometry rejected for production authority; `DATA_BLOCKED` |
| H | Changed geometry/permissions; revision comparison included |

## Test evidence

- `goal46-tests.log`: Goal46 geometry ingestion and readiness unit tests.
- `goal45-regression.log` and `goal45-scenario-regression.log`: fresh runs on this branch of the Goal45 ingestion and scenario-generation suites.
- `goal42-historical-regression.log`, `goal43-historical-regression.log`, and `goal44-historical-regression.log`: copied, unmodified passing logs from Goal45's earlier dedicated worktrees. Goal42-44 production modules are no longer present in this branch; `goal42-module-availability.log` records the unavailable-module check, and the historical suites are not represented as fresh Goal46-branch runs.
- Release configure/build/CTest logs and their result summary are recorded alongside this README.
- `determinism-results.json` records independent byte comparison runs for fixture and KAUO outputs.
- `SHA256SUMS.txt` covers every evidence file except itself; the ZIP has a separate SHA-256 sidecar.

## Reproduction

From repository root, run the Goal46 and Goal45 Python commands reflected in the `.log` files. CMake Release configure/build and CTest commands are listed in `validation-results.json`. Fixture generation is deterministic through `python -m tools.airport_geometry_ingestion.fixtures`'s `write_fixtures` API. Create the portable bundle with:

```powershell
python -m tools.airport_geometry_ingestion bundle docs/goal46-evidence --output docs/goal46-evidence.zip
```
