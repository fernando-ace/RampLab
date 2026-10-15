"""KAUO source-backed geometry, fail-closed readiness, and inspection output.

This module deliberately keeps legacy chart-digitized points in a rejected
collection. They are useful for reviewing the old model, but they are not
promoted into operational geometry.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
from typing import Any, Iterable

from tools.airport_scenario_generation.geospatial import GeographicCoordinate, geographic_to_enu

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_ANCHORS = ROOT / "tools/airport_scenario_generation/kauo.anchors.json"
DEFAULT_LEGACY_GEOMETRY = ROOT / "tools/airport_scenario_generation/kauo_goal31.geometry.json"
DEFAULT_NASR_EXTRACT = ROOT / "tools/airport_data_ingestion/sources/faa_nasr_auo_2026-10-01.json"
DEFAULT_DATASET = ROOT / "tools/airport_data_ingestion/kauo_operational_geometry.json"
DEFAULT_REPORT = ROOT / "docs/goal45-evidence/readiness-report.json"
DEFAULT_SVG = ROOT / "docs/goal45-evidence/source-vs-canonical.svg"
CLASSIFICATIONS = {"AUTHORITATIVE", "SOURCE_DERIVED", "APPROXIMATE", "SYNTHETIC"}


def _dump(value: Any) -> bytes:
    return (json.dumps(value, ensure_ascii=False, sort_keys=True, indent=2, allow_nan=False) + "\n").encode("utf-8")


def _round(value: float) -> float:
    return round(float(value), 6)


def _point(geo: dict[str, Any], origin: GeographicCoordinate) -> list[float]:
    local = geographic_to_enu(GeographicCoordinate(
        float(geo["latitude_degrees"]), float(geo["longitude_degrees"]), origin.ellipsoid_height_m
    ), origin)
    return [_round(local.east_m), _round(local.north_m)]


def validate_nasr_extract(extract: dict[str, Any], anchors: dict[str, Any]) -> list[str]:
    """Check the reduced FAA raw-record extract against the committed anchor snapshot."""
    errors: list[str] = []
    files = extract.get("files", {})
    base_rows = files.get("APT_BASE.csv", {}).get("records", [])
    if len(base_rows) != 1:
        return [f"APT_BASE.csv must contain exactly one AUO record; found {len(base_rows)}"]
    base = base_rows[0]
    origin = anchors.get("origin", {})
    for raw_field, anchor_field in (("LAT_DECIMAL", "latitude_degrees"), ("LONG_DECIMAL", "longitude_degrees"), ("ELEV", "field_elevation_ft_msl")):
        if float(base[raw_field]) != float(origin[anchor_field]):
            errors.append(f"APT_BASE.csv {raw_field} does not match anchor {anchor_field}")
    if base.get("ICAO_ID") != "KAUO" or base.get("ARPT_ID") != "AUO":
        errors.append("APT_BASE.csv record is not KAUO/AUO")
    for filename in ("APT_RWY.csv", "APT_RWY_END.csv"):
        if not files.get(filename, {}).get("records"):
            errors.append(f"{filename} has no extracted AUO records")
    runway_rows = {row["RWY_ID"]: row for row in files.get("APT_RWY.csv", {}).get("records", [])}
    endpoint_rows = {(row["RWY_ID"], row["RWY_END_ID"]): row
                     for row in files.get("APT_RWY_END.csv", {}).get("records", [])}
    for runway in anchors.get("runways", []):
        raw = runway_rows.get(runway["identifier"])
        if raw is None:
            errors.append(f"APT_RWY.csv is missing runway {runway['identifier']}")
            continue
        if abs(float(raw["RWY_LEN"]) * 0.3048 - float(runway["published_length_m"])) > 1e-6:
            errors.append(f"APT_RWY.csv length does not match runway {runway['identifier']}")
        if abs(float(raw["RWY_WIDTH"]) * 0.3048 - float(runway["published_width_m"])) > 1e-6:
            errors.append(f"APT_RWY.csv width does not match runway {runway['identifier']}")
        for end_id, anchor in runway["thresholds"].items():
            row = endpoint_rows.get((runway["identifier"], end_id))
            if row is None:
                errors.append(f"APT_RWY_END.csv is missing runway {runway['identifier']} end {end_id}")
                continue
            for raw_field, anchor_field in (("LAT_DECIMAL", "latitude_degrees"), ("LONG_DECIMAL", "longitude_degrees"), ("RWY_END_ELEV", "elevation_ft_msl")):
                if float(row[raw_field]) != float(anchor[anchor_field]):
                    errors.append(f"APT_RWY_END.csv {runway['identifier']} end {end_id} {raw_field} does not match anchor")
    return sorted(errors)


def _provenance(source_id: str, source_type: str, source_date: str | None,
                capture_method: str, classification: str, *, accuracy_m: float | None,
                confidence: float, simulation_approved: bool, notes: str) -> dict[str, Any]:
    if classification not in CLASSIFICATIONS:
        raise ValueError(f"unsupported geometry classification: {classification}")
    if not 0.0 <= confidence <= 1.0:
        raise ValueError("confidence must be between 0 and 1")
    if accuracy_m is not None and (not math.isfinite(accuracy_m) or accuracy_m < 0):
        raise ValueError("accuracy_m must be null or a finite nonnegative value")
    return {
        "source_id": source_id,
        "source_type": source_type,
        "source_date": source_date,
        "capture_method": capture_method,
        "classification": classification,
        "authoritative": classification == "AUTHORITATIVE",
        "accuracy_m": accuracy_m,
        "confidence": confidence,
        "simulation_approved": simulation_approved,
        "notes": notes,
    }


def build_dataset(anchors: dict[str, Any], legacy: dict[str, Any]) -> dict[str, Any]:
    """Create deterministic canonical geometry from FAA anchors and legacy records."""
    origin_row = anchors["origin"]
    origin = GeographicCoordinate(float(origin_row["latitude_degrees"]),
                                  float(origin_row["longitude_degrees"]),
                                  float(origin_row["ellipsoid_height_m"]))
    source_records = [
        {"source_id": "faa_nasr_2026_10_01", "publisher": "Federal Aviation Administration",
         "title": "28 Day NASR Subscription", "version_date": "2026-10-01",
         "url": "https://www.faa.gov/air_traffic/flight_info/aeronav/Aero_Data/NASR_Subscription/2026-10-01/",
         "data_type": "CSV aeronautical records", "crs": "WGS 84 geographic (EPSG:4979)",
         "accuracy_m": None,
         "features_supplied": ["airport reference point", "runway identifiers and dimensions", "runway-end coordinates"],
         "features_not_supplied": ["taxiway centerlines", "taxilanes", "apron boundaries", "stands", "service roads", "GSE locations", "crossing permissions"],
         "license_use": "FAA public aeronautical data; retain cycle and source fields. FAA does not publish horizontal accuracy in the cited NASR fields."},
        {"source_id": "faa_airport_diagram_2610", "publisher": "Federal Aviation Administration",
         "title": "KAUO Airport Diagram", "version_date": "2026-10-01/2026-10-28",
         "url": "https://aeronav.faa.gov/d-tpp/2610/05127AD.PDF",
         "data_type": "FAA airport diagram PDF", "crs": None, "accuracy_m": None,
         "features_supplied": ["charted runway/taxiway labels", "relative airfield arrangement", "hold-short markings as depicted"],
         "features_not_supplied": ["surveyed centerline coordinates", "stand coordinates", "service roads", "GSE locations", "operational crossing permissions"],
         "license_use": "Use as a chart for labels and diagrammatic relationships; digitized coordinates are approximate, not survey geometry."},
        {"source_id": "auburn_alp_board_packet_2015", "publisher": "Auburn University Board of Trustees",
         "title": "Auburn University Regional Airport Layout Plan, attachment in June 5, 2015 packet", "version_date": "2015-06-05 packet; drawing page says Page 3 of 4",
         "url": "https://www.auburn.edu/administration/trustees/materials/materials-20150605.pdf",
         "data_type": "PDF board packet drawing", "crs": None, "accuracy_m": None,
         "features_supplied": ["broad runway/taxiway/apron/hangar/property-plan context"],
         "features_not_supplied": ["verified current as-built state", "machine-readable coordinates", "stand coordinates", "vehicle access/clearance", "current operational rules"],
         "license_use": "Publicly accessible board packet; no redistribution rights assumed. The drawing/version and georeferencing are not established as current."},
        {"source_id": "auburn_terminal_ramp_expansion_2024", "publisher": "Auburn University Board of Trustees",
         "title": "Auburn University Regional Airport Terminal Ramp Expansion", "version_date": "2024-04-12",
         "url": "https://www.auburn.edu/administration/trustees/materials/materials-20240412.pdf",
         "data_type": "project memo and aerial site map", "crs": None, "accuracy_m": None,
         "features_supplied": ["proposed north terminal ramp expansion and tie-down-space quantities"],
         "features_not_supplied": ["as-built confirmation", "survey coordinates", "taxi routes", "stand dimensions", "vehicle clearance"],
         "license_use": "Public board packet; proposal/site map is not as-built geometry."},
        {"source_id": "aldot_apmp_auburn_2022", "publisher": "Alabama Department of Transportation Aeronautics Bureau",
         "title": "Alabama Statewide Airport Pavement Management Program Update, Auburn University Regional Airport Final Report", "version_date": "2022-02; visual pavement inspections November 2019; database ALDOT_210119",
         "url": "https://www.dot.state.al.us/programs/pdf/PavementProgram/Auburn.pdf",
         "data_type": "PAVER pavement inventory and condition report", "crs": None, "accuracy_m": None,
         "features_supplied": ["pavement branch identifiers, use classes, aggregate dimensions/areas and condition indices"],
         "features_not_supplied": ["feature coordinates/polygons", "operational centerlines", "stand positions", "clearances", "crossing rules"],
         "license_use": "ALDOT public report; condition and branch statistics do not authorize operational routing."},
        {"source_id": "aldot_apmp_gis", "publisher": "Alabama Department of Transportation Aeronautics Bureau",
         "title": "Aeronautics Airport Pavement Management GIS", "version_date": "accessed 2026-10-14",
         "url": "https://aldot.maps.arcgis.com/apps/webappviewer/index.html?id=ad7f40b4cbc94b499e69c518444a8a63",
         "data_type": "ArcGIS web application", "crs": "Not established from public viewer metadata in this review", "accuracy_m": None,
         "features_supplied": ["pavement program map and inventory context"],
         "features_not_supplied": ["verified operational centerline/stand/road data export", "survey accuracy and operational permissions"],
         "license_use": "Public viewer; no machine-readable AUO operational layer with suitable accuracy/provenance was obtained."},
        {"source_id": "aldot_grants_2026", "publisher": "Alabama Department of Transportation News Hub",
         "title": "Alabama Airport Improvement Grants Total $2 Million", "version_date": "2026-06-16",
         "url": "https://aldotnews.com/2026/06/16/alabama-airport-improvement-grants-total-2-million/",
         "data_type": "airport improvement grant table", "crs": None, "accuracy_m": None,
         "features_supplied": ["Auburn north terminal apron phases 5 and 6 listed as funded projects"],
         "features_not_supplied": ["as-built status", "coordinates", "taxiway/stand geometry", "operational permissions"],
         "license_use": "Public project-status information; funding/phase labels are not proof of construction completion."},
        {"source_id": "auburn_facilities_bid_documents", "publisher": "Auburn University Facilities Management",
         "title": "Public Works Bid Calendar and project documentation process", "version_date": "accessed 2026-10-14",
         "url": "https://fm.auburn.edu/contractor/",
         "data_type": "public procurement guidance", "crs": None, "accuracy_m": None,
         "features_supplied": ["identifies project plans/specifications as part of bid documentation"],
         "features_not_supplied": ["public downloadable KAUO CAD/GIS geometry without project enrollment and prequalification"],
         "license_use": "Official guidance states bidding documents are available to enrolled/prequalified contractors; this review did not access restricted project documents."},
        {"source_id": "legacy_goal31_geometry", "publisher": "RampLab Goal 25/31 project artifact",
         "title": "Legacy chart-graticule KAUO geometry and movement graph", "version_date": None,
         "url": "repository:tools/airport_scenario_generation/kauo_goal31.geometry.json",
         "data_type": "project JSON", "crs": "airport-local ENU after documented legacy-frame transform", "accuracy_m": None,
         "features_supplied": ["legacy approximate reference nodes and edges used only for rejection visualization"],
         "features_not_supplied": ["survey proof, operational validation, current surface state"],
         "license_use": "Project-owned model artifact; quarantined from simulation approval. Not an independent airport data source."},
    ]
    geometries: list[dict[str, Any]] = []
    airport_point = [0.0, 0.0]
    geometries.append({
        "geometry_id": "airport_reference_point", "feature_type": "airport_reference_point",
        "geometry_type": "Point", "coordinates": airport_point,
        "coordinate_reference_system": "RampLab KAUO airport-local ENU meters (+X east, +Y north)",
        "source_coordinates": {"latitude_degrees": origin.latitude_degrees, "longitude_degrees": origin.longitude_degrees},
        **_provenance("faa_nasr_2026_10_01", "FAA NASR APT_BASE.csv", "2026-10-01",
                      "FAA_PUBLISHED_POSITION", "AUTHORITATIVE", accuracy_m=None, confidence=0.95,
                      simulation_approved=True,
                      notes="NASR labels this airport reference position third-party survey dated 2006-05-22; no horizontal accuracy estimate is published."),
    })

    for runway in anchors["runways"]:
        end_ids = list(runway["thresholds"])
        threshold_coordinates: list[list[float]] = []
        for end_id in end_ids:
            row = runway["thresholds"][end_id]
            coordinates = _point(row, origin)
            threshold_coordinates.append(coordinates)
            geometries.append({
                "geometry_id": f"runway_{runway['identifier'].replace('/', '_')}_threshold_{end_id}",
                "feature_type": "runway_threshold", "geometry_type": "Point", "coordinates": coordinates,
                "runway_id": runway["identifier"], "threshold_id": end_id,
                "coordinate_reference_system": "RampLab KAUO airport-local ENU meters (+X east, +Y north)",
                "source_coordinates": {"latitude_degrees": row["latitude_degrees"], "longitude_degrees": row["longitude_degrees"],
                                       "elevation_ft_msl": row["elevation_ft_msl"], "vertical_datum": "MSL"},
                **_provenance("faa_nasr_2026_10_01", "FAA NASR APT_RWY_END.csv", "2006-05-22",
                              "FAA_PUBLISHED_POSITION", "AUTHORITATIVE", accuracy_m=None, confidence=0.95,
                              simulation_approved=True,
                              notes="FAA cycle record identifies a third-party survey; absolute horizontal accuracy is not published."),
            })
        measured_length = math.hypot(threshold_coordinates[1][0] - threshold_coordinates[0][0],
                                     threshold_coordinates[1][1] - threshold_coordinates[0][1])
        geometries.append({
            "geometry_id": f"runway_{runway['identifier'].replace('/', '_')}_centerline",
            "feature_type": "runway", "geometry_type": "LineString", "coordinates": threshold_coordinates,
            "runway_id": runway["identifier"], "published_length_m": runway["published_length_m"],
            "endpoint_distance_m": _round(measured_length), "width_m": runway["published_width_m"],
            "surface": runway["surface"],
            "coordinate_reference_system": "RampLab KAUO airport-local ENU meters (+X east, +Y north)",
            **_provenance("faa_nasr_2026_10_01", "Derived from FAA NASR APT_RWY and APT_RWY_END.csv", "2026-10-01",
                          "STRAIGHT_SEGMENT_BETWEEN_PUBLISHED_THRESHOLDS", "SOURCE_DERIVED", accuracy_m=None,
                          confidence=0.9, simulation_approved=True,
                          notes="Centerline is a straight segment between FAA-published runway ends. It does not define runway pavement shoulders, blast pads, declared distances, or surveyed pavement edges."),
        })

    legacy_airport = legacy["airport"]
    rejected: list[dict[str, Any]] = []
    for node in legacy_airport["nodes"]:
        rejected.append({
            "geometry_id": f"legacy_node_{node['id']}", "feature_type": "legacy_node",
            "geometry_type": "Point", "coordinates": [_round(node["x_m"]), _round(node["y_m"])],
            "legacy_id": node["id"], "coordinate_reference_system": "RampLab KAUO airport-local ENU meters (+X east, +Y north)",
            **_provenance("legacy_goal31_geometry", "RampLab Goal 25/31 JSON", None,
                          "CHART_GRATICULE_OR_ASSUMED_LEGACY_COORDINATE", "APPROXIMATE", accuracy_m=None,
                          confidence=0.25, simulation_approved=False,
                          notes=node.get("classification", "Legacy node has no per-feature source statement; treated as unsupported approximate geometry.")),
        })
    legacy_positions = {row["id"]: [float(row["x_m"]), float(row["y_m"])] for row in legacy_airport["nodes"]}
    for edge in legacy_airport["edges"]:
        from_id, to_id = edge["from"], edge["to"]
        if from_id not in legacy_positions or to_id not in legacy_positions:
            raise ValueError(f"legacy edge {edge['id']} references a missing node")
        rejected.append({
            "geometry_id": f"legacy_edge_{edge['id']}", "feature_type": "legacy_graph_edge",
            "geometry_type": "LineString", "coordinates": [legacy_positions[from_id], legacy_positions[to_id]],
            "legacy_id": edge["id"], "from_node": from_id, "to_node": to_id,
            "legacy_distance_m": edge.get("distance_m"),
            "coordinate_reference_system": "RampLab KAUO airport-local ENU meters (+X east, +Y north)",
            **_provenance("legacy_goal31_geometry", "RampLab Goal 25/31 JSON", None,
                          "LEGACY_GRAPH_CONNECTION", "APPROXIMATE", accuracy_m=None, confidence=0.1,
                          simulation_approved=False,
                          notes="Graph edge is quarantined with its endpoints; route length/topology has no current surveyed surface support."),
        })

    inventory = [
        {"feature_type": name, "geometry_count": count, "status": status, "notes": notes}
        for name, count, status, notes in [
            ("runway", 2, "available", "Two centerlines derived from FAA-published endpoints and dimensions."),
            ("runway_threshold", 4, "available", "Four FAA-published runway-end positions; accuracy not published."),
            ("taxiway_centerline", 0, "missing", "FAA diagram gives labels and relative layout only; no suitable source coordinates obtained."),
            ("taxiway_intersection", 0, "missing", "No current surveyed/GIS intersection coordinates."),
            ("taxilane", 0, "missing", "No current operational centerline data obtained."),
            ("apron_boundary", 0, "missing", "Public project drawings are schematic/proposed or old; no current as-built geospatial boundary obtained."),
            ("aircraft_stand", 0, "missing", "No current surveyed stand/parking coordinates or identifiers obtained."),
            ("service_road", 0, "missing", "No airside service road network or access restrictions obtained."),
            ("gse_staging_position", 0, "missing", "No verified GSE staging/depot locations obtained."),
            ("terminal_service_area", 0, "missing", "Airport operator confirms FBO service, but not service-area footprint or curb geometry."),
            ("hold_short_location", 0, "missing", "Diagram depicts markings, but exact surveyed coordinates and aircraft-specific stop geometry are not present."),
            ("runway_crossing", 0, "missing", "Chart warning requires crossing clearance; no operational permissions/logic data obtained."),
        ]
    ]
    return {
        "schema_version": "1.0",
        "airport_id": "KAUO",
        "dataset_id": "kauo_authoritative_geometry_goal45",
        "generated_by": "tools.airport_data_ingestion.kauo_geometry.build_dataset",
        "generation_policy": "Deterministic transform from committed FAA NASR anchor snapshot; all legacy chart/assumption geometry remains rejected and non-simulation-approved.",
        "coordinate_reference_system": {
            "source": "WGS 84 geographic 3D (EPSG:4979)",
            "simulation": "local ENU meters, +X east, +Y north, +Z up",
            "origin": {"latitude_degrees": origin.latitude_degrees, "longitude_degrees": origin.longitude_degrees,
                       "ellipsoid_height_m": origin.ellipsoid_height_m, "source_id": "faa_nasr_2026_10_01"},
            "transform": "WGS-84 geodetic to ECEF then tangent-plane ENU at airport reference point; see tools/airport_scenario_generation/geospatial.py.",
            "vertical_datum": "Source endpoint elevations are FAA-published feet MSL; spatial coordinates use the existing project ellipsoid-height origin. No independent geoid model is applied to endpoint positions.",
        },
        "sources": sorted(source_records, key=lambda row: row["source_id"]),
        "features": sorted(geometries, key=lambda row: row["geometry_id"]),
        "rejected_features": sorted(rejected, key=lambda row: row["geometry_id"]),
        "feature_inventory": inventory,
        "required_runway_ids": ["11/29", "18/36"],
        "operational_graph": {"nodes": [], "edges": [], "status": "unavailable", "source_id": None},
        "operational_relationships": [],
        "clearance_assessments": [],
        "readiness_requirements": [
            "taxiway_topology", "arrival_runway_exit_connectivity", "departure_runway_entry_connectivity",
            "gate_stand_connectivity", "turnaround_service_access", "gse_spawn_accessibility",
            "stationary_gse_clearance", "aircraft_route_clearance", "service_road_availability",
            "runway_crossing_definitions", "geometry_provenance", "geometry_confidence",
            "unsupported_inferred_attachments",
        ],
    }


def validate_provenance(dataset: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    seen: set[str] = set()
    provenance_rows: list[tuple[str, dict[str, Any]]] = []
    for collection in ("features", "rejected_features"):
        provenance_rows.extend((collection, feature) for feature in dataset.get(collection, []))
    graph = dataset.get("operational_graph", {})
    provenance_rows.extend(("operational_graph.nodes", row) for row in graph.get("nodes", []))
    provenance_rows.extend(("operational_graph.edges", row) for row in graph.get("edges", []))
    for collection, feature in provenance_rows:
        identity = feature.get("geometry_id", feature.get("node_id", feature.get("edge_id")))
        if not identity or identity in seen:
            errors.append(f"{collection}: missing or duplicate geometry_id {identity!r}")
        seen.add(identity)
        for key in ("source_id", "source_type", "source_date", "capture_method", "classification", "authoritative", "accuracy_m", "confidence", "simulation_approved", "notes"):
            if key not in feature:
                errors.append(f"{identity}: missing provenance field {key}")
        if feature.get("classification") not in CLASSIFICATIONS:
            errors.append(f"{identity}: invalid classification {feature.get('classification')!r}")
        if feature.get("authoritative") is not (feature.get("classification") == "AUTHORITATIVE"):
            errors.append(f"{identity}: authoritative flag does not match classification")
        if not isinstance(feature.get("simulation_approved"), bool):
            errors.append(f"{identity}: simulation_approved must be boolean")
        if feature.get("accuracy_m") is not None and (not isinstance(feature["accuracy_m"], (int, float)) or feature["accuracy_m"] < 0):
            errors.append(f"{identity}: accuracy_m must be null or nonnegative")
        confidence = feature.get("confidence")
        if not isinstance(confidence, (int, float)) or not 0 <= confidence <= 1:
            errors.append(f"{identity}: confidence must be between 0 and 1")
        elif feature.get("simulation_approved") and confidence < 0.75:
            errors.append(f"{identity}: simulation-approved geometry confidence is below the 0.75 gate")
        if feature.get("classification") in {"APPROXIMATE", "SYNTHETIC"} and feature.get("simulation_approved"):
            errors.append(f"{identity}: approximate/synthetic geometry cannot be simulation approved")
        if feature.get("source_id") not in {row["source_id"] for row in dataset.get("sources", [])}:
            errors.append(f"{identity}: unknown source_id {feature.get('source_id')!r}")
        if collection in {"features", "rejected_features"}:
            for key in ("coordinate_reference_system", "geometry_type", "coordinates"):
                if key not in feature:
                    errors.append(f"{identity}: missing geometry field {key}")
            coordinates = feature.get("coordinates")
            if feature.get("geometry_type") == "Point":
                points = [coordinates]
            elif feature.get("geometry_type") == "LineString":
                points = coordinates if isinstance(coordinates, list) else []
            else:
                points = []
                errors.append(f"{identity}: unsupported geometry_type {feature.get('geometry_type')!r}")
            if not points:
                errors.append(f"{identity}: geometry must contain at least one coordinate")
            for point in points:
                if (not isinstance(point, list) or len(point) != 2
                        or any(not isinstance(value, (int, float)) or not math.isfinite(value) for value in point)):
                    errors.append(f"{identity}: coordinates must be finite 2D numeric points")
    return sorted(errors)


def connected_components(nodes: Iterable[str], edges: Iterable[dict[str, Any]]) -> list[list[str]]:
    adjacency = {str(node): set() for node in nodes}
    for edge in edges:
        start, end = edge.get("from"), edge.get("to")
        if start not in adjacency or end not in adjacency:
            raise ValueError(f"edge {edge.get('id')} references a missing graph node")
        adjacency[start].add(end)
        adjacency[end].add(start)
    components: list[list[str]] = []
    unseen = set(adjacency)
    while unseen:
        seed = min(unseen)
        stack = [seed]
        found: set[str] = set()
        while stack:
            current = stack.pop()
            if current in found:
                continue
            found.add(current)
            stack.extend(sorted(adjacency[current] - found, reverse=True))
        unseen -= found
        components.append(sorted(found))
    return sorted(components, key=lambda component: (component[0], len(component)))


def point_segment_distance(point: tuple[float, float], start: tuple[float, float], end: tuple[float, float]) -> float:
    px, py = point
    ax, ay = start
    bx, by = end
    dx, dy = bx - ax, by - ay
    denom = dx * dx + dy * dy
    t = 0.0 if denom == 0 else max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / denom))
    return math.hypot(px - (ax + t * dx), py - (ay + t * dy))


def audit_clearances(assessments: list[dict[str, Any]]) -> list[dict[str, Any]]:
    results = []
    for row in sorted(assessments, key=lambda item: item.get("assessment_id", "")):
        required = row.get("required_clearance_m")
        measured = row.get("measured_clearance_m")
        if (required is None or measured is None or not row.get("source_id")
                or not row.get("assessment_method") or row.get("simulation_approved") is not True
                or not isinstance(row.get("confidence"), (int, float))
                or not 0 <= row.get("confidence", -1) <= 1):
            status = "UNKNOWN"
        elif not all(math.isfinite(float(value)) for value in (required, measured)):
            status = "UNKNOWN"
        else:
            status = "PASS" if float(measured) >= float(required) else "FAIL"
        results.append({"assessment_id": row.get("assessment_id", ""), "status": status,
                        "required_clearance_m": required, "measured_clearance_m": measured,
                        "source_id": row.get("source_id")})
    return results


def audit_readiness(dataset: dict[str, Any], *, authoritative_mode: bool = True) -> dict[str, Any]:
    findings: list[dict[str, Any]] = []
    errors = validate_provenance(dataset)
    for error in errors:
        findings.append({"code": "INVALID_PROVENANCE", "severity": "BLOCKER", "detail": error})
    if authoritative_mode:
        forbidden = [row["geometry_id"] for row in dataset.get("features", [])
                     if row.get("classification") in {"APPROXIMATE", "SYNTHETIC"}]
        if forbidden:
            findings.append({"code": "NON_AUTHORITATIVE_CANONICAL_GEOMETRY", "severity": "BLOCKER",
                             "detail": "Authoritative mode rejects approximate/synthetic canonical features: " + ", ".join(sorted(forbidden))})
    counts = {kind: sum(1 for row in dataset.get("features", []) if row.get("feature_type") == kind)
              for kind in ("runway", "runway_threshold", "taxiway_centerline", "taxiway_intersection", "aircraft_stand", "service_road", "gse_staging_position", "hold_short_location", "runway_crossing")}
    active_features = dataset.get("features", [])
    feature_ids = {row.get("geometry_id") for row in active_features}
    feature_counts = {kind: sum(1 for row in active_features if row.get("feature_type") == kind)
                      for kind in {row.get("feature_type") for row in active_features}}
    relationships = dataset.get("operational_relationships", [])
    valid_relationships = []
    unsupported_relationships = []
    source_ids = {row.get("source_id") for row in dataset.get("sources", [])}
    for relation in relationships:
        endpoints = (relation.get("from_feature_id"), relation.get("to_feature_id"))
        usable = (relation.get("simulation_approved") is True
                  and relation.get("source_id") in source_ids
                  and all(endpoint in feature_ids for endpoint in endpoints)
                  and not relation.get("inferred", False))
        (valid_relationships if usable else unsupported_relationships).append(relation)
    relation_types = {row.get("relation_type") for row in valid_relationships}

    def need(code: str, missing: bool, detail: str) -> None:
        if missing:
            findings.append({"code": code, "severity": "BLOCKER", "detail": detail})

    graph_nodes = dataset.get("operational_graph", {}).get("nodes", [])
    nodes = [row.get("node_id") for row in graph_nodes]
    edges = dataset.get("operational_graph", {}).get("edges", [])
    expected_runways = set(dataset.get("required_runway_ids", []))
    actual_runways = {row.get("runway_id") for row in active_features if row.get("feature_type") == "runway"}
    endpoints_by_runway: dict[str, set[str]] = {}
    for row in active_features:
        if row.get("feature_type") == "runway_threshold":
            endpoints_by_runway.setdefault(row.get("runway_id"), set()).add(row.get("threshold_id"))
    runway_topology_missing = not expected_runways or not expected_runways.issubset(actual_runways)
    for runway_id in expected_runways:
        if not set(runway_id.split("/")).issubset(endpoints_by_runway.get(runway_id, set())):
            runway_topology_missing = True
    need("RUNWAY_TOPOLOGY_INCOMPLETE", runway_topology_missing,
         "Runway topology requires each declared runway centerline and its two matching source-backed threshold points.")
    need("TAXIWAY_TOPOLOGY_MISSING", feature_counts.get("taxiway_centerline", 0) == 0
         or feature_counts.get("taxiway_intersection", 0) == 0 or not edges,
         "No source-backed taxiway centerlines, intersections, and connected graph edges are available.")
    need("ARRIVAL_RUNWAY_EXIT_CONNECTIVITY_MISSING", "arrival_runway_exit_connectivity" not in relation_types,
         "No source-backed arrival runway-exit attachment to the taxiway graph is available.")
    need("DEPARTURE_RUNWAY_ENTRY_CONNECTIVITY_MISSING", "departure_runway_entry_connectivity" not in relation_types,
         "No source-backed departure runway-entry route from the taxiway graph is available.")
    need("GATE_STAND_CONNECTIVITY_MISSING", feature_counts.get("aircraft_stand", 0) == 0
         or "gate_stand_connectivity" not in relation_types,
         "No current source-backed aircraft stand coordinates or stand-to-taxiway connections are available.")
    need("TURNAROUND_SERVICE_ACCESS_MISSING", "turnaround_service_access" not in relation_types,
         "No verified service-area and turnaround access geometry is available.")
    need("GSE_SPAWN_ACCESSIBILITY_MISSING", feature_counts.get("gse_staging_position", 0) == 0
         or "gse_spawn_accessibility" not in relation_types,
         "No verified GSE staging locations or access graph are available.")
    assessment_types = {row.get("assessment_type") for row in dataset.get("clearance_assessments", [])}
    need("STATIONARY_GSE_CLEARANCE_UNKNOWN", "stationary_gse" not in assessment_types,
         "No source-backed GSE positions and stationary vehicle clearance assessment are available.")
    need("AIRCRAFT_ROUTE_CLEARANCE_UNKNOWN", "aircraft_route" not in assessment_types,
         "No operational route geometry and aircraft swept-clearance assessment are available.")
    need("SERVICE_ROAD_NETWORK_MISSING", feature_counts.get("service_road", 0) == 0
         or "service_road_access" not in relation_types,
         "No source-backed airside service-road network or restriction/access data are available.")
    need("RUNWAY_CROSSING_RULES_MISSING", feature_counts.get("runway_crossing", 0) == 0
         or "runway_crossing_permission" not in relation_types,
         "The FAA diagram warns that runway crossing clearance is required; no crossing geometry and permissions are available.")
    graph_unusable = [row.get("node_id", row.get("edge_id")) for row in graph_nodes + edges
                      if row.get("simulation_approved") is not True
                      or row.get("classification") in {"APPROXIMATE", "SYNTHETIC"}
                      or row.get("source_id") not in source_ids]
    if graph_unusable:
        findings.append({"code": "UNSUPPORTED_GRAPH_GEOMETRY", "severity": "BLOCKER",
                         "detail": "Operational graph contains unapproved, approximate, synthetic, or unsourced geometry: " + ", ".join(map(str, sorted(graph_unusable)))})
    if not nodes:
        findings.append({"code": "OPERATIONAL_GRAPH_MISSING", "severity": "BLOCKER", "detail": "The source-backed operational graph has no nodes or edges."})
        components: list[list[str]] = []
    else:
        components = connected_components(nodes, edges)
        if len(components) > 1:
            findings.append({"code": "DISCONNECTED_GRAPH_COMPONENTS", "severity": "BLOCKER",
                             "detail": f"Operational graph has {len(components)} disconnected components.", "components": components})
    clearance_results = audit_clearances(dataset.get("clearance_assessments", []))
    for result in clearance_results:
        if result["status"] != "PASS":
            findings.append({"code": "CLEARANCE_" + result["status"], "severity": "BLOCKER",
                             "detail": f"Clearance assessment {result['assessment_id']} is {result['status']}.", "assessment_id": result["assessment_id"]})
    if not clearance_results:
        findings.append({"code": "CLEARANCE_ASSESSMENTS_MISSING", "severity": "BLOCKER", "detail": "No aircraft-route or stationary-GSE clearance results exist."})
    if unsupported_relationships:
        findings.append({"code": "UNSUPPORTED_INFERRED_ATTACHMENTS", "severity": "BLOCKER",
                         "detail": f"{len(unsupported_relationships)} operational relationships use inferred, unapproved, unsourced, or non-canonical feature attachments."})
    findings.sort(key=lambda finding: (finding["code"], finding.get("detail", "")))
    status = "READY" if not any(row["severity"] == "BLOCKER" for row in findings) else "BLOCKED"
    return {
        "report_schema_version": "1.0", "airport_id": dataset.get("airport_id"),
        "status": status, "banner": f"KAUO DATA READINESS: {status}",
        "authoritative_mode": authoritative_mode,
        "feature_counts": {"canonical": counts, "rejected": len(dataset.get("rejected_features", []))},
        "graph_component_count": len(components), "clearance_assessments": clearance_results,
        "findings": findings,
        "simulation_authorized": status == "READY",
        "simulation_decision": "RUN PERMITTED BY DATA GATE" if status == "READY" else "DATA BLOCKED: production KAUO simulator execution prohibited by readiness gate",
    }


def render_svg(dataset: dict[str, Any]) -> str:
    """Render deterministic schematic in local ENU; no raster is treated as georeferenced."""
    rows = dataset.get("features", [])
    rejected = dataset.get("rejected_features", [])
    positions = []
    for row in rows + rejected:
        coordinates = row.get("coordinates", [])
        if row.get("geometry_type") == "Point":
            positions.append(coordinates)
        else:
            positions.extend(coordinates)
    if not positions:
        raise ValueError("dataset has no geometry to render")
    min_x, max_x = min(p[0] for p in positions), max(p[0] for p in positions)
    min_y, max_y = min(p[1] for p in positions), max(p[1] for p in positions)
    pad = 60
    width, height = 1200, 820
    scale = min((width - 2 * pad) / max(max_x - min_x, 1.0), 550 / max(max_y - min_y, 1.0))
    def xy(point: list[float]) -> tuple[float, float]:
        return pad + (point[0] - min_x) * scale, height - 140 - pad - (point[1] - min_y) * scale
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" role="img" aria-labelledby="title desc">',
           '<title id="title">KAUO source-backed geometry and rejected legacy approximation</title>',
           '<desc id="desc">FAA runway threshold points, derived centerlines, and legacy approximate graph shown in airport-local ENU. Not a georeferenced FAA chart overlay.</desc>',
           '<rect width="100%" height="100%" fill="#fbfaf6"/>',
           '<text x="40" y="42" font-family="Arial,sans-serif" font-size="24" font-weight="700" fill="#18212b">KAUO source vs canonical geometry</text>',
           '<text x="40" y="70" font-family="Arial,sans-serif" font-size="14" fill="#46515b">Local ENU meters · source survey accuracy unpublished · diagram-digitalized legacy graph rejected</text>',
           '<g stroke="#d8d9d5" stroke-width="1">']
    for tick in range(-1000, 1300, 250):
        x, y = xy([tick, min_y])
        if 30 < x < width - 30:
            out.append(f'<line x1="{x:.1f}" y1="100" x2="{x:.1f}" y2="{height-170}"/>')
        x, y = xy([min_x, tick])
        if 100 < y < height - 170:
            out.append(f'<line x1="40" y1="{y:.1f}" x2="{width-40}" y2="{y:.1f}"/>')
    out.append('</g>')
    # Rejected edges are shown first, so source-backed runway features stay visible.
    out.append('<g fill="none" stroke="#d27b20" stroke-width="1.4" stroke-dasharray="5 5" opacity="0.56">')
    for row in rejected:
        if row["geometry_type"] == "LineString":
            a, b = (xy(point) for point in row["coordinates"])
            out.append(f'<line x1="{a[0]:.2f}" y1="{a[1]:.2f}" x2="{b[0]:.2f}" y2="{b[1]:.2f}"/>')
    out.append('</g><g fill="#fff4e5" stroke="#bd6412" stroke-width="1.1" opacity="0.85">')
    for row in rejected:
        if row["geometry_type"] == "Point":
            x, y = xy(row["coordinates"])
            out.append(f'<circle cx="{x:.2f}" cy="{y:.2f}" r="3.8"/>')
    out.append('</g>')
    out.append('<g fill="none" stroke="#1f6f8b" stroke-width="10" stroke-linecap="round" opacity="0.8">')
    for row in rows:
        if row["feature_type"] == "runway":
            a, b = (xy(point) for point in row["coordinates"])
            out.append(f'<line x1="{a[0]:.2f}" y1="{a[1]:.2f}" x2="{b[0]:.2f}" y2="{b[1]:.2f}"/>')
    out.append('</g><g fill="#fff" stroke="#16485a" stroke-width="2">')
    for row in rows:
        if row["feature_type"] == "runway_threshold":
            x, y = xy(row["coordinates"])
            out.append(f'<circle cx="{x:.2f}" cy="{y:.2f}" r="6"/>')
    out.append('</g>')
    legend_y = height - 110
    out.extend([
        f'<line x1="45" y1="{legend_y}" x2="82" y2="{legend_y}" stroke="#1f6f8b" stroke-width="9"/><text x="92" y="{legend_y+5}" font-family="Arial" font-size="13">FAA threshold endpoints; centerlines derived from published coordinates</text>',
        f'<line x1="45" y1="{legend_y+25}" x2="82" y2="{legend_y+25}" stroke="#d27b20" stroke-width="2" stroke-dasharray="5 5"/><circle cx="63" cy="{legend_y+25}" r="4" fill="#fff4e5" stroke="#bd6412"/><text x="92" y="{legend_y+30}" font-family="Arial" font-size="13">Legacy approximate nodes/edges, rejected from simulation use ({len(rejected)} records)</text>',
        f'<text x="45" y="{height-32}" font-family="Arial" font-size="12" fill="#46515b">Chart layout is not georeferenced here. Approximate feature positions are shown only to expose the legacy model, not to validate it.</text>',
        '</svg>',
    ])
    return "\n".join(out) + "\n"


def write_deterministic(path: Path, payload: Any) -> str:
    content = payload if isinstance(payload, bytes) else _dump(payload)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(content)
    return hashlib.sha256(content).hexdigest()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--anchors", type=Path, default=DEFAULT_ANCHORS)
    parser.add_argument("--legacy-geometry", type=Path, default=DEFAULT_LEGACY_GEOMETRY)
    parser.add_argument("--nasr-extract", type=Path, default=DEFAULT_NASR_EXTRACT)
    parser.add_argument("--dataset", type=Path, default=DEFAULT_DATASET)
    parser.add_argument("--report", type=Path, default=DEFAULT_REPORT)
    parser.add_argument("--svg", type=Path, default=DEFAULT_SVG)
    parser.add_argument("--authoritative-mode", action="store_true", help="reject approximate/synthetic canonical features")
    parser.add_argument("--require-ready", action="store_true", help="exit 2 and do not authorize simulation when blocked")
    args = parser.parse_args(argv)
    anchors = json.loads(args.anchors.read_text(encoding="utf-8"))
    extract = json.loads(args.nasr_extract.read_text(encoding="utf-8"))
    extract_errors = validate_nasr_extract(extract, anchors)
    if extract_errors:
        raise SystemExit("FAA NASR source/anchor mismatch: " + "; ".join(extract_errors))
    dataset = build_dataset(anchors, json.loads(args.legacy_geometry.read_text(encoding="utf-8")))
    write_deterministic(args.dataset, dataset)
    report = audit_readiness(dataset, authoritative_mode=args.authoritative_mode)
    write_deterministic(args.report, report)
    args.svg.parent.mkdir(parents=True, exist_ok=True)
    args.svg.write_text(render_svg(dataset), encoding="utf-8", newline="\n")
    print(report["banner"])
    print(report["simulation_decision"])
    print(f"dataset={args.dataset}")
    print(f"report={args.report}")
    print(f"visualization={args.svg}")
    return 2 if args.require_ready and report["status"] != "READY" else 0


if __name__ == "__main__":
    raise SystemExit(main())
