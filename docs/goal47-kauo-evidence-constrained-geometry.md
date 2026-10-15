# Goal47: Evidence-Constrained KAUO Geometry Reconstruction

## Decision

**Status: `PARTIALLY_RECONSTRUCTED`. Production readiness remains `DATA_BLOCKED`.**

The evidence supports a coherent pavement inventory and a useful candidate taxiway centerline network. It does not support an operational KAUO route graph: source positional accuracy is unpublished, no aircraft or GSE permissions are established, widths and clearances are unavailable, topology has unresolved intersections and endpoints, and stand/runway-entry/runway-exit access is not represented. The simulator was not run because the production gate correctly blocks it. This is a source-backed partial reconstruction, not an airport digital twin or an approved operating map.

## Source investigation and retained artifacts

| Source | Use and classification | Date / CRS / resolution | Artifact |
|---|---|---|---|
| FAA d-TPP KAUO Airport Diagram, cycle 2610 | Authoritative published labels, runway identities and relative taxiway topology; not geodetic taxiway geometry | Effective 2026-10-01; diagram is not a georeferenced vector source | `sources/faa-kauo-airport-diagram-cycle-2610.pdf`, `sources/faa_kauo_diagram_manifest.json` |
| FAA NASR KAUO extract | Authoritative airport reference and runway/threshold records; FAA survey/source date 2006-05-22, horizontal accuracy not stated | Published extract 2026-10-01; WGS84 coordinates | `sources/faa_nasr_auo_2026-10-01.json` |
| Alabama DOT Aeronautics Airport Pavement Management GIS | Authoritative pavement inventory polygons for inventory purposes; 37 runway/taxiway/apron/other sections, not routes or access permissions | Layer in Web Mercator (EPSG:3857), transformed through WGS84 into local ENU; map references Appendix B (2021), service snapshot retrieved 2026-10-14 | `sources/aldot_auo_pavement_sections.geojson`, metadata and manifest JSON |
| USGS NAIP ImageServer | Public-domain aerial imagery used for visual context and alignment review, not survey control | Acquisition 2023-10-08; 0.6 m pixel size; four bands; exported crop EPSG:3857. Pixel size is not positional accuracy. | `sources/usgs-naip-2023-kauo-context.jpg` and JSON acquisition/query metadata |
| OpenStreetMap API | Supporting crowd-sourced aeroway geometry; 58 taxiway, 2 runway, and 3 apron ways, with referenced nodes | Retrieved 2026-10-15; EPSG:4326; ODbL 1.0 | `sources/osm-kauo-aeroway.osm` and source manifest; unrelated address/building data excluded |
| Auburn University Board of Trustees, 2024-04-12 | Planning reference for proposed north terminal ramp expansion and 15 tie-downs; does not establish completion or georeferenced geometry | Published 2024-04-12 | `sources/auburn_2024_terminal_ramp_expansion.json` |

The City of Auburn, Alabama aerial GIS service was investigated, but its reuse terms did not authorize retaining or redistributing its imagery, so it was not acquired. No organization was contacted and no paid data was obtained. Source URLs, identifiers, hashes, licensing, acquisition dates, selection methods, and limits are recorded in the per-source manifests and canonical dataset.

## Reconstruction and evidence

Goal46 schema 2.1 represents `AUTHORITATIVE`, `SOURCE_DERIVED`, `APPROXIMATE`, and `SYNTHETIC` separately. Source-derived provenance records confidence, accuracy, derivation configuration hash, corroborating sources, and validation checks. Production eligibility is fail-closed: source-derived routing geometry requires HIGH confidence, known accuracy at most 1 m, independent production-grade corroboration, and passing CRS/bounds/geometry/review checks. MEDIUM and LOW confidence cannot enter production routing. The 1 m threshold is an eligibility policy, not a claimed source accuracy.

ALDOT pavement polygons were explicitly mapped by pavement type and normalized into the airport-local ENU frame. OSM taxiway ways were transformed from WGS84 and checked against ALDOT pavement polygons. Fifty-five taxiway ways with vertices contained by mapped pavement are retained as `SOURCE_DERIVED / MEDIUM`, with unknown accuracy and permissions; three ways not fully contained are quarantined as `APPROXIMATE`. The 148 pre-existing legacy route features remain quarantined. The FAA airport diagram is used only for identity/relative-topology corroboration, not as a claim of geodetic centerline validation. NAIP is an imagery review layer; no manual digitizing or unsupported HIGH assignment is used.

Generated model counts: 99 active inventory/reference features and 151 quarantined features. This includes 42 AUTHORITATIVE features (1 airport reference point, 4 runway thresholds, 6 runway pavement polygons, 23 taxiway pavement polygons, 2 aprons, 6 other surface polygons), 57 SOURCE_DERIVED/MEDIUM features (2 runway axes and 55 taxiway candidate centerlines), zero SOURCE_DERIVED/HIGH or LOW, 151 APPROXIMATE quarantined features (148 legacy features plus 3 OSM lines), and zero SYNTHETIC operational features. The 55 candidate centerlines total 7,714.378 m by derived local-ENU segment length; this is a measurement of source coordinates, not a statement of positional accuracy. ALDOT polygon areas are recorded in `geometry/geometry-summary.json`.

The candidate topology diagnostic has 69 nodes, 55 edges, and 17 connected components. It reports 33 ambiguous intersections, 43 dangling endpoints, and unknown vehicle permissions on all 55 edges. It is shown for review only. The production aircraft and GSE graphs each have zero nodes and edges; stands, runway entries/exits, crossings, and approved routes are zero. The only simulation assumption is that candidate OSM lines are treated as undirected for connected-component diagnostics; it grants no vehicle movement permission and is not an airport operating rule.

The generated inventory map and NAIP overlay distinguish pavement classes, FAA runway axes, candidate taxiway centerlines, and quarantined legacy geometry. The overlay is contextual visual evidence only. Legacy point-in-polygon comparison finds 13 of 54 legacy routing nodes inside ALDOT mapped pavement and 41 outside; this does not validate route legality or safety and promotes no legacy feature.

## Readiness and remaining evidence

`geometry/readiness.md` records the production gate result. The airport reference point and provenance completeness pass. Runway geometry is blocked because runway identity/threshold pairing is incomplete under the production eligibility criteria. Taxiway geometry, connectivity, runway connections, stand access, GSE/service access, pushback/turnaround, runway crossing permissions, source authority, and graph validity remain blocked. Route clearances remain unknown. No blockers were converted to passes using visual assumptions. Since readiness is blocked, no simulator configuration, seed, route metrics, safety metrics, or simulation determinism claim applies.

## Validation and reproduction

All data generation is offline and deterministic. Reproduce the canonical model and reports with:

```powershell
python -m tools.airport_geometry_ingestion.goal47
python -m unittest tools.airport_geometry_ingestion.test_geometry tools.airport_geometry_ingestion.test_goal47 -v
```

Fresh Goal45, airport-ingestion, scenario-generation, and Release CTest command/results are recorded in `validation-results.json`. Results: 18 geometry/Goal47 tests, 30 airport-ingestion tests, 52 scenario-generation tests, and 229 Release CTest tests passed. Independent model builds produce byte-identical canonical dataset bytes; the canonical dataset hash is `56fa37f5176becef2ac7a8bde81d65333c624250347b5c9aa2d62b98d75b6cb0`. `SHA256SUMS.txt` covers the evidence directory; the portable ZIP has a separate SHA-256 sidecar.

## Recommended Goal48

Obtain or locate survey-grade/georeferenced airport geometry with stated accuracy and operational access/width attributes. Until then, keep Goal47's candidate network review-only. If approved source geometry becomes available, build explicit runway-entry, stand, GSE and service connections, resolve topology with evidence, and rerun the existing production gate before any KAUO simulation.
