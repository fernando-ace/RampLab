# RampLab airport geometry data requirements

RampLab needs current, georeferenced airport surface and access data before it can validate operational taxi, stand, service, or GSE movements. These requirements are for data intake only; a file is not treated as operationally valid until its source, currency, topology, permissions, widths, and clearances have been checked.

## Preferred delivery formats

1. GeoJSON or another documented GIS export with CRS metadata and stable feature IDs.
2. Shapefile or GeoPackage, including `.prj`/CRS definitions and associated metadata (the Goal46 importer currently requires conversion to a supported format before use).
3. CAD/DXF with documented drawing units, origin, axis/grid bearing, layers, and georeferencing (the current importer does not parse DXF directly).
4. Surveyed CSV point-and-edge network with explicit endpoint IDs, directionality, units, coordinate frame, and source IDs.
5. Another documented georeferenced source with its coordinate system and accuracy information.

Do not omit CRS definitions, coordinate units, drawing scale/origin, or field meaning. A screenshot or an unreferenced PDF is not operational geometry.

## Required layers and attributes

- Airport reference point; coordinate system and datum; field elevation and vertical datum when available.
- Runway identifiers, both runway-end/threshold positions, centerline or pavement boundary/width, and declared dimensions where supplied.
- Taxiway/taxilane centerlines or boundaries, stable identifiers, widths where available, intersections, hold-short positions, and entry/exit relationships to runways.
- Apron/ramp polygons with surface/use classification when known.
- Aircraft stands with stable stand IDs, position, heading/orientation and dimensions where known, aircraft access link, and pushback/turnaround connectivity.
- GSE staging/depot/access locations, service roads, vehicle-only links, and explicit aircraft/GSE shared-surface relationships.
- Runway crossing geometry and explicit crossing permission/clearance metadata. A physical intersection does not establish permission.
- Restricted/protected areas, non-traversable regions, operational restrictions, and source-defined clearance constraints.
- Widths or boundaries needed to check aircraft/GSE envelopes and route separation.

For each dataset, provide source/custodian, publication or survey date, revision/cycle, original feature IDs, accuracy/precision statement, extraction method, intended use, known limitations, and permitted reuse. Keep source lineage for derived features and identify any geometry that is approximate, proposed, inferred, or not current.

## Readiness inputs

The simulator readiness gate also requires source-backed runway entry/exit connectivity, stand-to-network access, arrival and departure routes, turnaround/pushback access, service-road/GSE access, explicit crossing permissions for any route that crosses a runway, and clearance evidence. Missing or unpublished measurements remain `UNKNOWN`; they are never assigned a passing value by default.

The intake pipeline accepts GeoJSON, explicit CSV point/edge records, KML/KMZ, and canonical RampLab schema 2.0 JSON. Other GIS/CAD formats should be converted with a documented tool and mapping before import; the original file and its hash should be retained alongside the conversion record.
