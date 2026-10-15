"""Normalize ALDOT's Auburn pavement GIS into the KAUO airport-local frame.

The source feature service describes pavement inventory sections, not certified
taxiway centerlines or gate coordinates. This module keeps that distinction in
the machine-readable output.
"""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
from typing import Any

from .geospatial import GeographicCoordinate, geographic_to_enu
from .surface_routes import derive_surface_routes

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "tools" / "airport_scenario_generation" / "sources" / "aldot_auo_pavement_sections.geojson"
OUTPUT = ROOT / "tools" / "airport_scenario_generation" / "kauo_goal42.geometry.json"
BASE = ROOT / "tools" / "airport_scenario_generation" / "kauo_goal31.geometry.json"
ORIGIN = GeographicCoordinate(32.61511111, -85.434, 208.22)

SOURCE_RECORD = {
    "source": "Alabama Department of Transportation Aeronautics Bureau, Airport Pavement Management Program GIS",
    "source_url": "https://services.arcgis.com/LZzQi3xDiclG6XvQ/arcgis/rest/services/Section_241016/FeatureServer/0",
    "program_url": "https://www.dot.state.al.us/programs/AirportPavementMaintenance.html",
    "source_identifier": "NETWORKID=AUO; Section_241016/FeatureServer/0",
    "source_layer_crs": "Web Mercator (WKID 102100 / EPSG:3857)",
    "download_crs": "WGS 84 longitude/latitude (EPSG:4326; ArcGIS query outSR=4326)",
    "acquisition_date": "2026-10-14",
    "dataset_date": "ALDOT program says 2018 study; pavement condition records include 2019 survey dates; hosted layer last edited 2024-10-17",
    "transformation": "WGS 84 geographic coordinates to airport-local ENU meters using the existing Goal 26 KAUO ARP origin and WGS-84 ECEF/topocentric conversion",
    "classification": "SOURCE_BACKED_DERIVED",
    "limitations": [
        "Pavement inventory sections are not an FAA-certified movement-area survey or taxiway centerline dataset.",
        "Some broad taxiway/taxilane and unclassified OTHER polygons describe pavement inventory extents and must not be interpreted as an aircraft centerline.",
        "The 2D source has no vertical coordinates; elevations are unknown.",
        "The layer is a 2018 study with later hosted edits; it is not guaranteed to describe current pavement condition or operational status.",
        "No gate/stand positions or GSE staging positions are surveyed by this source."
    ]
}


def _polygon_parts(geometry: dict[str, Any]) -> list[list[list[float]]]:
    kind = geometry.get("type")
    coordinates = geometry.get("coordinates")
    if kind == "Polygon" and isinstance(coordinates, list):
        return [coordinates]
    if kind == "MultiPolygon" and isinstance(coordinates, list):
        return coordinates
    raise ValueError(f"unsupported ALDOT geometry type: {kind!r}")


def _local_point(point: list[float]) -> dict[str, float]:
    if len(point) < 2:
        raise ValueError("surface coordinate must contain longitude and latitude")
    lon, lat = float(point[0]), float(point[1])
    if not (-180 <= lon <= 180 and -90 <= lat <= 90):
        raise ValueError("surface coordinate is outside EPSG:4326 bounds")
    # Source height is absent. Reusing the ARP ellipsoid height only defines the
    # horizontal tangent frame; generated surface Z remains explicitly unknown.
    local = geographic_to_enu(GeographicCoordinate(lat, lon, ORIGIN.ellipsoid_height_m), ORIGIN)
    return {"x_m": round(local.east_m, 3), "y_m": round(local.north_m, 3)}


def normalize_source(source_path: Path = SOURCE) -> dict[str, Any]:
    """Read and validate source pavement sections, returning normalized polygons."""
    raw = source_path.read_bytes()
    try:
        document = json.loads(raw.decode("utf-8-sig"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ValueError("ALDOT source must be a UTF-8 GeoJSON document") from exc
    if document.get("type") != "FeatureCollection" or not isinstance(document.get("features"), list):
        raise ValueError("ALDOT source must be a GeoJSON FeatureCollection")

    areas: list[dict[str, Any]] = []
    seen_ids: set[str] = set()
    for feature in document["features"]:
        properties = feature.get("properties")
        if not isinstance(properties, dict):
            raise ValueError("ALDOT feature is missing properties")
        object_id = properties.get("OBJECTID", feature.get("id"))
        if object_id is None:
            raise ValueError("ALDOT feature has no stable source identifier")
        area_id = f"aldot_auo_{object_id}"
        if area_id in seen_ids:
            raise ValueError(f"duplicate ALDOT source feature identifier: {area_id}")
        seen_ids.add(area_id)
        geometry = feature.get("geometry")
        if not isinstance(geometry, dict):
            raise ValueError(f"ALDOT feature {area_id} has no geometry")
        parts = _polygon_parts(geometry)
        normalized_parts = []
        for polygon in parts:
            if not isinstance(polygon, list) or not polygon:
                raise ValueError(f"ALDOT feature {area_id} has an empty polygon")
            rings = []
            for ring in polygon:
                if len(ring) < 4 or ring[0][:2] != ring[-1][:2]:
                    raise ValueError(f"ALDOT feature {area_id} has an open or degenerate ring")
                rings.append([_local_point(point) for point in ring])
            normalized_parts.append(rings)
        use = str(properties.get("USE_", "")).strip().upper()
        classification = use if use in {"RUNWAY", "TAXIWAY", "APRON"} else "UNCLASSIFIED_OTHER"
        areas.append({
            "id": area_id,
            "source_feature_id": str(object_id),
            "name": str(properties.get("BRANCHNAME", "")).strip() or None,
            "branch_id": str(properties.get("BRANCHID", "")).strip() or None,
            "use": classification,
            "section_id": str(properties.get("SECTIONID", "")).strip() or None,
            "surface": str(properties.get("SURFACE", "")).strip() or None,
            "inventory_length_ft": properties.get("LENGTH"),
            "inventory_width_ft": properties.get("WIDTH"),
            "inventory_area_sqft": properties.get("AREA"),
            "geometry_type": geometry["type"],
            "polygons": normalized_parts,
            "classification": "SOURCE_BACKED_DERIVED" if classification != "UNCLASSIFIED_OTHER" else "SOURCE_RECORDED_UNCLASSIFIED",
            "source": "ALDOT_AUO_PAVEMENT_GIS"
        })
    if not areas:
        raise ValueError("ALDOT source contains no pavement features")
    return {"source_sha256": hashlib.sha256(raw).hexdigest(), "surface_areas": sorted(areas, key=lambda item: item["id"])}


def _inside(point: tuple[float, float], ring: list[dict[str, float]]) -> bool:
    x, y = point
    inside = False
    for left, right in zip(ring, ring[1:]):
        x1, y1 = left["x_m"], left["y_m"]
        x2, y2 = right["x_m"], right["y_m"]
        if (y1 > y) != (y2 > y) and x < (x2 - x1) * (y - y1) / (y2 - y1) + x1:
            inside = not inside
    return inside


def graph_alignment_audit(airport: dict[str, Any], surface_areas: list[dict[str, Any]], samples_per_edge: int = 101) -> dict[str, Any]:
    """Measure route centerline samples inside classified ALDOT pavement polygons."""
    nodes = {node["id"]: node for node in airport.get("nodes", [])}
    areas = [area for area in surface_areas if area["use"] in {"RUNWAY", "TAXIWAY", "APRON"}]
    results = []
    for edge in airport.get("edges", []):
        line = edge.get("surface_centerline") or [nodes[edge["from"]], nodes[edge["to"]]]
        segment_lengths = [((right["x_m"] - left["x_m"]) ** 2 + (right["y_m"] - left["y_m"]) ** 2) ** 0.5
                           for left, right in zip(line, line[1:])]
        total = sum(segment_lengths)
        covered = 0
        for index in range(samples_per_edge):
            remaining = total * index / (samples_per_edge - 1)
            segment_index = 0
            while segment_index < len(segment_lengths) - 1 and remaining > segment_lengths[segment_index]:
                remaining -= segment_lengths[segment_index]
                segment_index += 1
            left, right = line[segment_index], line[segment_index + 1]
            fraction = 0.0 if segment_lengths[segment_index] == 0 else remaining / segment_lengths[segment_index]
            point = (left["x_m"] + (right["x_m"] - left["x_m"]) * fraction,
                     left["y_m"] + (right["y_m"] - left["y_m"]) * fraction)
            if any(_inside(point, polygon[0]) and not any(_inside(point, hole) for hole in polygon[1:])
                   for area in areas for polygon in area["polygons"]):
                covered += 1
        coverage = covered / samples_per_edge
        results.append({"edge_id": edge["id"], "from": edge["from"], "to": edge["to"],
                        "surface_polygon_coverage_ratio": round(coverage, 6),
                        "status": "MOSTLY_WITHIN_INVENTORY" if coverage >= 0.9 else
                                  "PARTIAL_OVERLAP" if coverage >= 0.5 else "OUTSIDE_OR_LOW_OVERLAP"})
    return {"method": f"{samples_per_edge} evenly spaced points on each generated centerline tested against ALDOT runway/taxiway/apron outer rings",
            "interpretation": "derived raster paths are constrained to classified pavement inventory interiors; inventory extents do not establish approved centerlines or legal routing",
            "edge_count": len(results), "mostly_within_count": sum(row["status"] == "MOSTLY_WITHIN_INVENTORY" for row in results),
            "edges": results}


def build_geometry(base_path: Path = BASE, source_path: Path = SOURCE) -> dict[str, Any]:
    base = json.loads(base_path.read_text(encoding="utf-8-sig"))
    normalized = normalize_source(source_path)
    airport = base.get("airport")
    if not isinstance(airport, dict):
        raise ValueError("base geometry has no airport object")
    airport["surface_areas"] = normalized["surface_areas"]
    airport["surface_routing_model"] = derive_surface_routes(airport)
    airport["graph_alignment_audit"] = graph_alignment_audit(airport, normalized["surface_areas"])
    airport["geometry_provenance"] = {
        "runway_reference_points": {
            "source": "FAA NASR 28-day subscription, APT_RWY_END.csv; Goal 26 anchors",
            "classification": "AUTHORITATIVE_SOURCE_RECORDS",
            "crs": "WGS 84 geographic 3D (EPSG:4979)",
            "limitations": ["FAA source survey date is 2006-05-22; published decimal precision is not an accuracy guarantee."]
        },
        "runway_centerlines_and_intersection": {
            "source": "Derived from FAA NASR surveyed runway-end coordinates",
            "classification": "DERIVED",
            "limitations": ["Centerline, heading, and intersection are calculations from endpoint records."]
        },
        "runway_taxiway_apron_pavement_areas": {
            **SOURCE_RECORD,
            "snapshot_sha256": normalized["source_sha256"],
            "feature_count": len(normalized["surface_areas"]),
            "classified_counts": {
                use: sum(1 for area in normalized["surface_areas"] if area["use"] == use)
                for use in ("RUNWAY", "TAXIWAY", "APRON", "UNCLASSIFIED_OTHER")
            }
        },
        "route_graph": {
            "source": "Deterministic 2 m raster A* through FAA/ALDOT-aligned operating-pavement inventory areas",
            "classification": "SOURCE_BACKED_DERIVED_WITH_MANUALLY_RECONCILED_APPROXIMATIONS",
            "limitations": ["ALDOT inventory polygons do not define approved taxiway centerlines or legal routing; approximate node attachment displacements are retained per node."]
        },
        "stands_and_gates": {
            "source": "Illustrative simulator mapping; no surveyed stand coordinates obtained",
            "classification": "APPROXIMATE",
            "limitations": ["Gate IDs, stand locations, and stand service approaches are scenario assumptions."]
        },
        "service_areas": {
            "source": "Illustrative Goal 41 ground-unit staging and service-point mapping",
            "classification": "APPROXIMATE",
            "limitations": ["GSE depots, service points, fleet inventory, and access routes are simulation assumptions."]
        }
    }
    features = airport.setdefault("features", {})
    features["geometry_status"] = "FAA NASR runway-end controls; ALDOT airport-pavement inventory polygons for runways, taxiways and aprons; derived route graph; approximate stands, service points and operational semantics."
    features["surface_area_source_classification"] = "SOURCE_BACKED_DERIVED (ALDOT GIS pavement inventory; not survey-grade taxiway centerlines)"
    features["apron_classification"] = "SOURCE_BACKED_DERIVED apron pavement polygons; individual stands remain APPROXIMATE"
    return {
        "schema_version": 1,
        "airport": airport,
        "gates": base.get("gates", []),
        "surface_operations": base.get("surface_operations", {}),
        "calibration": base.get("calibration", {}),
        "source_snapshot_sha256": normalized["source_sha256"]
    }


def write_geometry(output_path: Path = OUTPUT) -> dict[str, Any]:
    geometry = build_geometry()
    output_path.write_text(json.dumps(geometry, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return geometry


if __name__ == "__main__":
    result = write_geometry()
    print(json.dumps({"output": str(OUTPUT), "surface_area_count": len(result["airport"]["surface_areas"]),
                      "source_snapshot_sha256": result["source_snapshot_sha256"]}, sort_keys=True))
