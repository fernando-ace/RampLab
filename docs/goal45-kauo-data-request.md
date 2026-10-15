# KAUO operational geometry data request

**Purpose:** complete a source-backed, non-operational digital-twin model for Auburn University Regional Airport (KAUO). RampLab does not need access-control details, security plans, flight schedules, or sensitive infrastructure information.

**Suggested recipient:** Auburn University Regional Airport, `airport@auburn.edu`, or the airport's current airport-engineering/planning consultant. Auburn University Facilities Management's Public Works Bid Calendar is another source for existing project drawings; its public guidance says enrollment and project-specific prequalification are required for bid documents.

## Requested material

1. **Current FAA-approved Airport Layout Plan (ALP):** the approved plan sheets, effective/revision date, FAA approval/adoption status, and a clear distinction between existing, proposed, and removed facilities.
2. **Available machine-readable survey/design data:** CAD, GIS, shapefile, GeoPackage, GeoJSON, DXF/DWG, KML, or equivalent. A 2D export is sufficient if it preserves the source coordinate system and feature identifiers.
3. **Airside geometry:** current as-built runway and taxiway pavement boundaries; taxiway and taxilane centerlines; intersections/connectors; apron boundaries; and any currently used aircraft parking/stand/tie-down reference points and orientation.
4. **Vehicle access geometry, if already maintained:** airside service roads, access restrictions, GSE/service staging areas, and FBO/terminal service areas. RampLab does not need fleet identities, routes, schedules, or security-sensitive staging details.
5. **Movement constraints needed to model surface connectivity:** surveyed runway entry/exit attachment points, hold-short positions, runway crossing geometry, and a clear statement of crossing/hold-short permissions or the public rule source governing them.
6. **Recent project status:** whether north terminal-area ramp phases 5 and 6, the 2024 terminal-ramp expansion, and other relevant airside work are proposed, under construction, or complete; provide the corresponding as-built drawing only if available.

## Metadata needed with the files

- Horizontal and vertical CRS/datum, units, axis order, and any transformation already applied.
- Survey/acquisition date, source organization, capture method, stated horizontal/vertical accuracy, and whether each feature is design, proposed, or as-built.
- Feature identifiers, names, surface/use attributes, and any aircraft dimension/weight restrictions attached to the geometry.
- Permission/license for RampLab to use and retain the supplied geometry in a local, non-operational research prototype. Public distribution is not required.
- A contact for questions about ambiguities or superseded/removed features.

If only PDF plan sheets are available, a georeferenced export or coordinate table is preferred. A raster or unreferenced drawing can help identify labels and proposed work but cannot by itself establish simulation-grade coordinates.

## Why each item is needed

The current public FAA NASR record supports runway references, but its cited third-party survey is dated 2006 and includes no horizontal accuracy estimate. FAA diagrams identify taxiways and show relative layout, while public ALP and pavement reports do not supply a current machine-readable operational graph. RampLab therefore reports `DATA BLOCKED` until it can verify taxiway topology, runway connections, stands, service access, crossings, provenance/confidence, and route/vehicle clearances from appropriate source data.

This is a prepared request only. No email or external message was sent.
