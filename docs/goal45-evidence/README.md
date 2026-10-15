# Goal45 KAUO geometry evidence

Research and validation date: 2026-10-14. The source register and request form are in `../goal45-kauo-source-register.md` and `../goal45-kauo-data-request.md`.

## Decision

The readiness gate returns **DATA BLOCKED** in authoritative mode. It authorizes no production KAUO simulator run. The canonical dataset contains the FAA airport reference point, four FAA runway threshold points, and two runway centerlines derived from those points. It contains no taxiway graph, stands, service roads, crossing geometry or permissions, GSE positions, or clearance assessments. The 148 legacy chart-digitized graph features remain quarantined as approximate and not simulation approved.

## Included artifacts

- `kauo_operational_geometry.json`: canonical dataset and quarantined legacy features.
- `faa_nasr_auo_2026-10-01.json`: AUO rows extracted from the FAA NASR APT CSV group. The FAA archive itself is retained locally under ignored `tmp/goal45-pdfs/` and is described with hashes in `source-artifact-manifest.json`.
- `readiness-report.json`: authoritative-mode gate findings; `simulation_authorized` is false.
- `source-vs-canonical.svg`: review rendering of source-derived and rejected legacy geometry.
- `source-artifact-manifest.json`: source URLs, local-retention paths, file sizes, and SHA-256 values. Downloaded public source documents are not redistributed here.
- `validation-results.json`: regression, build, CTest, readiness, and determinism results.
- `SHA256SUMS.txt`: hashes for the portable evidence files (excluding this checksum file and the ZIP archive).

## Reproduction

From the repository root, run the Python commands listed in `validation-results.json`, then configure and build with CMake and run `ctest --test-dir build-goal45-release -C Release --output-on-failure`. The project uses the Visual Studio generator on this Windows host, so `--config Release` selects the build configuration; `CMAKE_BUILD_TYPE` is ignored by that generator.

The existing Goal42, Goal43, and Goal44 regression tests were run from their own worktrees, without copying or importing their generated geometry into Goal45. Their results are recorded separately in `validation-results.json`.
