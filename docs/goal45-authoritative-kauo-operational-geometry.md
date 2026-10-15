# Goal45 authoritative KAUO operational geometry

## Finding

**DATA BLOCKED.** FAA data supports the KAUO airport reference point and four runway-end locations. The available public sources do not provide current, survey-quality taxiway/taxilane geometry, stands, service access, GSE staging, runway crossing permissions, or the clearances needed to validate production routes. The production simulator was not run because the data-readiness gate did not pass.

## Canonical model

`tools/airport_data_ingestion/kauo_operational_geometry.json` is a deterministic, source-backed spatial dataset alongside the existing tabular ingestion package. Each geometry record carries:

- stable geometry ID and feature type;
- geometry type and coordinates in the declared airport-local frame;
- source ID/type/date and capture method;
- `AUTHORITATIVE`, `SOURCE_DERIVED`, `APPROXIMATE`, or `SYNTHETIC` classification;
- an explicit `authoritative` flag, source accuracy in meters (or `null` when unpublished), source-confidence score, simulation approval, and explanatory notes.

Confidence describes confidence in source attribution/classification. It does not substitute for positional accuracy. `accuracy_m: null` means the source did not state an accuracy estimate.

The four FAA runway-end positions are stored as authoritative 2D points in local ENU, retaining source latitude/longitude and published elevation in feet MSL. Runway centerlines are straight segments derived between each pair of FAA-published threshold coordinates; FAA-published length and width are retained as attributes. The airport reference point is the ENU origin. The source height in the pre-existing FAA anchor file includes an approximate geoid separation for compatibility; Goal45 makes no new claim about vertical accuracy. Ground geometry is horizontal, and threshold elevations remain in the published MSL datum rather than being mixed with ellipsoid height.

The coordinate transform reuses `tools/airport_scenario_generation/geospatial.py`: WGS-84 geodetic coordinates are converted to ECEF and then to a local tangent ENU frame with +X east, +Y north, +Z up, in meters. No image-based or nearest-pavement adjustment is applied. Round-trip behavior and representative FAA points are covered by existing geospatial tests; Goal45 verifies the published NASR values against a committed extract of the raw AUO rows.

## Provenance classes and inventory

The generated canonical dataset contains 7 features: 1 airport reference point, 4 authoritative threshold points, and 2 source-derived runway centerlines. There are 54 prior chart/assumption nodes and 94 prior graph links in `rejected_features`. Their classification is `APPROXIMATE`, their simulation approval is false, and their legacy data source is identified per record. No synthetic operational feature was generated.

No canonical taxiway, intersection, taxilane, apron-boundary, aircraft-stand, service-road, service-area, GSE-staging, hold-short, or runway-crossing geometry is present. These are represented as zero-count inventory rows with missing status. The FAA diagram's labels and relative map layout are qualitative reference only. The Goal31 graph remains viewable as rejected legacy geometry so reviewers can see what is quarantined; it is not treated as an additional airport source.

See [`goal45-kauo-source-register.md`](goal45-kauo-source-register.md) for the public-source inventory, dates, limitations, access path to project plan documents, and source hierarchy review. See [`goal45-kauo-data-request.md`](goal45-kauo-data-request.md) for the exact additional inputs needed.

## Fail-closed readiness gate

Run the deterministic data build, report and visualization with:

```powershell
python -m tools.airport_data_ingestion.kauo_geometry --authoritative-mode
```

The command validates the FAA extract against the anchor snapshot, emits the canonical dataset and a stable readiness report, and generates an SVG source/canonical comparison. `--require-ready` returns exit code 2 if the report is blocked, allowing a production wrapper to stop before invoking the simulator. Authoritative mode rejects any approximate or synthetic geometry in the active canonical collection; the rejected collection is retained solely for inspection.

The readiness audit checks:

- runway IDs and matching threshold points;
- taxiway centerlines, intersections and graph edges;
- arrival runway exits and departure runway entries;
- stand connectivity and turnaround service access;
- GSE staging/access and stationary vehicle clearance;
- aircraft-route clearance;
- service roads and access where required;
- runway crossing geometry/permissions;
- disconnected graph components;
- provenance, source confidence and unsupported inferred/unapproved attachments.

Relationships and clearance measurements must cite a source and be explicitly approved. Missing or unknown values block the gate; they are not converted to zero or a pass. A test-only complete fixture proves that a fully populated source-backed candidate can pass the gate. It is labeled as test data and does not contribute to the KAUO dataset.

The actual KAUO gate result is recorded in [`goal45-evidence/readiness-report.json`](goal45-evidence/readiness-report.json). The exact blockers are:

- incomplete runway exit/entry safety topology beyond threshold/centerline references;
- no taxiway centerlines/intersections or operational graph;
- no arrival exit or departure entry connections;
- no current stands or stand-to-taxiway links;
- no service area or turnaround access graph;
- no verified GSE spawn locations/access graph;
- no stationary-GSE or aircraft-route clearance results;
- no service-road network/access data;
- no runway crossing geometry and permission/hold-short rules.

## Visual review

[`goal45-evidence/source-vs-canonical.svg`](goal45-evidence/source-vs-canonical.svg) overlays FAA-derived runway axes/threshold points with the legacy approximate node/edge network in local ENU meters. Dashed orange legacy features are explicitly rejected. The FAA diagram itself is not georeferenced into the plot; the image is a spatial review of published and existing project geometry, not a chart tracing or accuracy claim.

## Validation and decision

`tools/airport_data_ingestion/test_kauo_geometry.py` checks deterministic data generation and reports, raw NASR consistency, required provenance, classification restrictions, positive and negative gate paths, missing data, graph connectivity, clearance status and point-to-segment distance. Focused scenario-generation and legacy regression suites plus the full Release CTest are recorded in `goal45-evidence/`.

Because the readiness result is `BLOCKED`, no Goal42 production control/disruption scenario or simulator replay was executed. This follows the Goal45 simulator gate. No collision, turnaround, safety, or simulator-pass claim is made for Goal45.
