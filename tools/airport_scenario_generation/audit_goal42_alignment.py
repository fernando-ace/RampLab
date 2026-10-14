"""Quantify KAUO route-graph support by ALDOT operating-pavement polygons.

This dependency-light audit does not infer a rigid transform or relax a tolerance.
It tests the coordinates as stored, so a route-geometry problem cannot be hidden
by moving the airport or by changing the acceptance threshold.
"""
from __future__ import annotations

import argparse
import json
import math
import statistics
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_GEOMETRY = ROOT / "tools/airport_scenario_generation/kauo_goal42.geometry.json"
DEFAULT_SCENARIO = ROOT / "docs/goal42-evidence/run/generated/control/scenario.json"
DEFAULT_EVENTS = ROOT / "docs/goal42-evidence/run/control/events.jsonl"
DEFAULT_OUTPUT = ROOT / "docs/goal42-evidence/geometry"


def _contains(point: tuple[float, float], ring: list[dict[str, float]]) -> bool:
    x, y = point
    inside = False
    for a, b in zip(ring, ring[1:]):
        x1, y1, x2, y2 = a["x_m"], a["y_m"], b["x_m"], b["y_m"]
        if (y1 > y) != (y2 > y) and x < (x2 - x1) * (y - y1) / (y2 - y1) + x1:
            inside = not inside
    return inside


def _segment_distance(point: tuple[float, float], a: dict[str, float], b: dict[str, float]) -> float:
    dx, dy = b["x_m"] - a["x_m"], b["y_m"] - a["y_m"]
    denominator = dx * dx + dy * dy
    fraction = 0.0 if denominator == 0 else max(0.0, min(1.0,
        ((point[0] - a["x_m"]) * dx + (point[1] - a["y_m"]) * dy) / denominator))
    return math.hypot(point[0] - a["x_m"] - fraction * dx,
                      point[1] - a["y_m"] - fraction * dy)


class SurfaceIndex:
    def __init__(self, areas: list[dict[str, Any]]):
        self.polygons: list[tuple[dict[str, Any], list[list[dict[str, float]]], tuple[float, float, float, float], list[tuple[dict[str, float], dict[str, float]]]]] = []
        for area in areas:
            for polygon in area["polygons"]:
                outer = polygon[0]
                bounds = (min(p["x_m"] for p in outer), min(p["y_m"] for p in outer),
                          max(p["x_m"] for p in outer), max(p["y_m"] for p in outer))
                segments = [(a, b) for ring in polygon for a, b in zip(ring, ring[1:])]
                self.polygons.append((area, polygon, bounds, segments))

    def nearest(self, point: tuple[float, float], uses: set[str] | None = None) -> tuple[float, dict[str, Any] | None]:
        nearest_distance = math.inf
        nearest_area = None
        x, y = point
        for area, polygon, (xmin, ymin, xmax, ymax), segments in self.polygons:
            if uses and area["use"] not in uses:
                continue
            if max(xmin - x, 0.0, x - xmax, ymin - y, 0.0, y - ymax) > nearest_distance:
                continue
            if _contains(point, polygon[0]) and not any(_contains(point, hole) for hole in polygon[1:]):
                return 0.0, area
            distance = min((_segment_distance(point, a, b) for a, b in segments), default=math.inf)
            if distance < nearest_distance:
                nearest_distance, nearest_area = distance, area
        return nearest_distance, nearest_area


def _node_position(node: dict[str, Any]) -> tuple[float, float]:
    return float(node["x_m"]), float(node["y_m"])


def _edge_samples(start: tuple[float, float], end: tuple[float, float], count: int) -> list[tuple[float, float]]:
    return [(start[0] + i / (count - 1) * (end[0] - start[0]),
             start[1] + i / (count - 1) * (end[1] - start[1])) for i in range(count)]


def _production_edge_use(scenario_path: Path, events_path: Path) -> dict[str, int]:
    scenario = json.loads(scenario_path.read_text(encoding="utf-8"))
    nodes = scenario["airport"]["nodes"]
    node_ids = {index + 1: node["id"] for index, node in enumerate(nodes)}
    edge_ids = {(edge["from"], edge["to"]): edge["id"] for edge in scenario["airport"]["edges"]}
    used: dict[str, int] = {}
    with events_path.open(encoding="utf-8") as stream:
        for line in stream:
            event = json.loads(line)
            if event.get("type") != "SurfaceTaxiRouteAssigned":
                continue
            route = [node_ids[int(value)] for value in event.get("route_nodes", [])]
            for a, b in zip(route, route[1:]):
                edge_id = edge_ids.get((a, b), edge_ids.get((b, a)))
                if edge_id:
                    used[edge_id] = used.get(edge_id, 0) + 1
    return used


def _edge_audit(airport: dict[str, Any], index: SurfaceIndex, used: dict[str, int], samples: int) -> list[dict[str, Any]]:
    nodes = {node["id"]: _node_position(node) for node in airport["nodes"]}
    rows = []
    for edge in airport["edges"]:
        points = _edge_samples(nodes[edge["from"]], nodes[edge["to"]], samples)
        nearest = [index.nearest(point, {"RUNWAY", "TAXIWAY", "APRON"}) for point in points]
        distances = sorted(value[0] for value in nearest)
        maximum_index = max(range(len(nearest)), key=lambda i: nearest[i][0])
        distances_taxi_apron = sorted(index.nearest(point, {"TAXIWAY", "APRON"})[0] for point in points)
        row = {
            "edge_id": edge["id"], "from": edge["from"], "to": edge["to"],
            "edge_classification": edge.get("classification", "UNCLASSIFIED"),
            "production_route_assignments": used.get(edge["id"], 0),
            "sample_count": samples,
            "nearest_operating_surface_distance_m": {
                "minimum": round(distances[0], 3), "mean": round(statistics.mean(distances), 3),
                "p95": round(distances[min(samples - 1, math.ceil(0.95 * samples) - 1)], 3),
                "maximum": round(distances[-1], 3),
                "samples_within_1m": sum(value <= 1.0 for value in distances),
                "samples_within_5m": sum(value <= 5.0 for value in distances),
                "samples_within_10m": sum(value <= 10.0 for value in distances),
                "maximum_distance_sample_index": maximum_index,
                "nearest_feature_at_maximum": nearest[maximum_index][1]["id"] if nearest[maximum_index][1] else None,
                "nearest_feature_use_at_maximum": nearest[maximum_index][1]["use"] if nearest[maximum_index][1] else None,
            },
            "nearest_taxiway_or_apron_distance_m": {
                "minimum": round(distances_taxi_apron[0], 3),
                "p95": round(distances_taxi_apron[min(samples - 1, math.ceil(0.95 * samples) - 1)], 3),
                "maximum": round(distances_taxi_apron[-1], 3),
                "samples_within_10m": sum(value <= 10.0 for value in distances_taxi_apron),
            },
        }
        rows.append(row)
    return rows


def _make_overlay(airport: dict[str, Any], path: Path, used: dict[str, int]) -> None:
    from PIL import Image, ImageDraw, ImageFont

    width, height = 1800, 1300
    areas = airport["surface_areas"]
    all_points = [point for area in areas for polygon in area["polygons"] for ring in polygon for point in ring]
    graph_nodes = {node["id"]: _node_position(node) for node in airport["nodes"]}
    all_points.extend({"x_m": p[0], "y_m": p[1]} for p in graph_nodes.values())
    xmin, xmax = min(p["x_m"] for p in all_points), max(p["x_m"] for p in all_points)
    ymin, ymax = min(p["y_m"] for p in all_points), max(p["y_m"] for p in all_points)
    pad_m = 110
    xmin -= pad_m; xmax += pad_m; ymin -= pad_m; ymax += pad_m
    margin = 90
    scale = min((width - 2 * margin) / (xmax - xmin), (height - 2 * margin) / (ymax - ymin))

    def xy(point: dict[str, float] | tuple[float, float]) -> tuple[float, float]:
        x = point["x_m"] if isinstance(point, dict) else point[0]
        y = point["y_m"] if isinstance(point, dict) else point[1]
        return margin + (x - xmin) * scale, height - margin - (y - ymin) * scale

    image = Image.new("RGB", (width, height), "#f6f8fb")
    draw = ImageDraw.Draw(image)
    palette = {"RUNWAY": (135, 149, 164, 255), "TAXIWAY": (76, 174, 132, 255),
               "APRON": (242, 187, 82, 255), "UNCLASSIFIED_OTHER": (176, 185, 195, 255)}
    overlay = Image.new("RGBA", image.size, (0, 0, 0, 0)); od = ImageDraw.Draw(overlay)
    for area in areas:
        color = palette.get(area["use"], palette["UNCLASSIFIED_OTHER"])
        for polygon in area["polygons"]:
            outer = [xy(point) for point in polygon[0]]
            od.polygon(outer, fill=color, outline=(38, 60, 78, 255))
            for hole in polygon[1:]:
                od.polygon([xy(point) for point in hole], fill=(246, 248, 251, 255))
    image = Image.alpha_composite(image.convert("RGBA"), overlay).convert("RGB")
    draw = ImageDraw.Draw(image)
    try:
        font = ImageFont.truetype("arial.ttf", 20)
        small = ImageFont.truetype("arial.ttf", 15)
        title = ImageFont.truetype("arial.ttf", 29)
    except OSError:
        font = small = title = ImageFont.load_default()
    for edge in airport["edges"]:
        classification = edge.get("classification", "")
        if classification.startswith(("APPROXIMATE GSE", "ASSUMED FOR SIMULATION GSE",
                                      "ASSUMED FOR SIMULATION short service")):
            continue
        a, b = xy(graph_nodes[edge["from"]]), xy(graph_nodes[edge["to"]])
        color, stroke = ((195, 42, 56), 5) if edge["id"] in used else ((50, 69, 91), 2)
        draw.line((a, b), fill=color, width=stroke)
    labeled_nodes = {"rwy18_36_mid", "rwy36_hold", "rwy18_threshold", "rwy11_threshold",
                     "rwy29_threshold", "runway_intersection", "twy_a_south", "twy_a_mid",
                     "twy_a_north", "fbo_apron_junction", "fbo_apron_detour",
                     "ga_parking_stand", "ga_parking_stand_02", "ga_parking_stand_03",
                     "self_serve_fuel_farm"}
    for node in airport["nodes"]:
        if node["id"] not in labeled_nodes:
            continue
        x, y = xy(graph_nodes[node["id"]])
        draw.ellipse((x - 5, y - 5, x + 5, y + 5), fill="#ffffff", outline="#243b53", width=2)
        draw.text((x + 7, y - 17), node["id"], fill="#142b43", font=small)
    draw.text((45, 24), "KAUO ALDOT pavement inventory and Goal42 route graph", font=title, fill="#122b45")
    draw.text((47, 62), "Coordinates: airport-local ENU meters, +X east / +Y north. Red = production-used graph edges; dark = other facility-graph edges.", font=small, fill="#334e68")
    y = 100
    for use, color in palette.items():
        draw.rectangle((48, y, 70, y + 17), fill=color[:3], outline="#263746")
        draw.text((78, y), use.replace("_", " "), font=small, fill="#243b53")
        y += 25
    scale_length = 100.0 * scale
    sx, sy = width - margin - scale_length, height - 42
    draw.line((sx, sy, sx + scale_length, sy), fill="#122b45", width=5)
    draw.line((sx, sy - 8, sx, sy + 8), fill="#122b45", width=3)
    draw.line((sx + scale_length, sy - 8, sx + scale_length, sy + 8), fill="#122b45", width=3)
    draw.text((sx, sy - 28), "100 m", font=small, fill="#122b45")
    draw.text((48, height - 43), "GSE staging/service spokes are hidden for readability; they remain included in the JSON distance audit.",
              font=small, fill="#334e68")
    path.parent.mkdir(parents=True, exist_ok=True)
    image.save(path)


def audit(geometry_path: Path = DEFAULT_GEOMETRY, scenario_path: Path = DEFAULT_SCENARIO,
          events_path: Path = DEFAULT_EVENTS, output_dir: Path = DEFAULT_OUTPUT,
          samples: int = 101) -> dict[str, Any]:
    document = json.loads(geometry_path.read_text(encoding="utf-8"))
    airport = document["airport"]
    if samples < 3:
        raise ValueError("at least three samples per edge are required")
    used = _production_edge_use(scenario_path, events_path)
    valid_areas = [a for a in airport["surface_areas"] if a["use"] in {"RUNWAY", "TAXIWAY", "APRON"}]
    index = SurfaceIndex(valid_areas)
    edges = _edge_audit(airport, index, used, samples)
    max_distance = sorted(row["nearest_operating_surface_distance_m"]["maximum"] for row in edges)
    route_edges = [row for row in edges if row["production_route_assignments"]]
    route_max = sorted(row["nearest_operating_surface_distance_m"]["maximum"] for row in route_edges)
    nodes = []
    runway_index = SurfaceIndex([area for area in airport["surface_areas"] if area["use"] == "RUNWAY"])
    runway_reference_checks = []
    for node in airport["nodes"]:
        distance, area = index.nearest(_node_position(node))
        nodes.append({"node_id": node["id"], "classification": node.get("classification", "UNCLASSIFIED"),
                      "nearest_operating_surface_distance_m": round(distance, 3),
                      "nearest_surface_id": area["id"] if area else None,
                      "nearest_surface_use": area["use"] if area else None})
        if node["id"] in {"rwy18_threshold", "rwy11_threshold", "rwy29_threshold", "rwy36_hold"}:
            runway_distance, runway_area = runway_index.nearest(_node_position(node))
            runway_reference_checks.append({
                "node_id": node["id"],
                "reference": "FAA runway-end anchor" if node["id"] != "rwy36_hold" else "FAA Runway 36-end anchor reused by the taxi-hold node",
                "nearest_ALDOT_runway_polygon_distance_m": round(runway_distance, 3),
                "nearest_ALDOT_runway_feature_id": runway_area["id"] if runway_area else None,
            })
    report = {
        "schema_version": 1,
        "method": {
            "coordinate_system": "WGS 84 geographic (EPSG:4326) source polygons transformed to FAA KAUO ENU meters (EPSG:4979 origin); graph and polygons audited without post-hoc alignment",
            "source_layer_original_crs": "Web Mercator (WKID 102100 / EPSG:3857)",
            "source_query_crs": "WGS 84 longitude/latitude (EPSG:4326)",
            "origin": airport["calibration"]["origin"],
            "surfaces": "ALDOT outer boundaries and interior rings; aircraft-operable RUNWAY, TAXIWAY, and APRON classifications only",
            "distance": "minimum Euclidean point-to-polygon distance in airport-local ENU meters; inside a valid polygon is 0 m",
            "edge_sampling": f"{samples} evenly spaced samples including each endpoint per straight graph edge",
            "known_transform_limitation": "ALDOT service query supplied 4326 geometry; original 3857 geometry was not separately acquired for an independent projection round-trip check",
        },
        "counts": {"route_edges": len(airport["edges"]), "route_nodes": len(airport["nodes"]),
                   "valid_operating_surface_features": len(valid_areas),
                   "valid_operating_surface_polygon_parts": len(index.polygons),
                   "production_route_edges_used": len(route_edges),
                   "production_route_assignment_count": sum(used.values())},
        "faa_runway_anchor_to_aldot_runway_polygon_checks": runway_reference_checks,
        "all_graph_edge_max_distance_distribution_m": {
            "minimum": min(max_distance), "median": statistics.median(max_distance),
            "mean": round(statistics.mean(max_distance), 3),
            "p95": max_distance[min(len(max_distance) - 1, math.ceil(0.95 * len(max_distance)) - 1)],
            "maximum": max(max_distance),
            "edges_with_max_distance_at_most_1m": sum(value <= 1 for value in max_distance),
            "edges_with_max_distance_at_most_10m": sum(value <= 10 for value in max_distance),
            "edges_with_max_distance_at_most_25m": sum(value <= 25 for value in max_distance),
        },
        "production_edge_max_distance_distribution_m": {
            "minimum": min(route_max) if route_max else None,
            "median": statistics.median(route_max) if route_max else None,
            "mean": round(statistics.mean(route_max), 3) if route_max else None,
            "p95": route_max[min(len(route_max) - 1, math.ceil(0.95 * len(route_max)) - 1)] if route_max else None,
            "maximum": max(route_max) if route_max else None,
            "edges_with_max_distance_at_most_1m": sum(value <= 1 for value in route_max),
            "edges_with_max_distance_at_most_10m": sum(value <= 10 for value in route_max),
        },
        "edges": edges,
        "nodes": nodes,
        "interpretation": {
            "crs_or_origin_diagnosis": "The near-zero Taxiway A alignment and runway anchors near FAA/ALDOT pavement argue against a single large translation or axis-sign error. The major production discrepancies are localized to approximate apron access, assumed stand locations, and service connectors. This is a consistency diagnosis, not a survey-accuracy claim.",
            "outlier_edge_ids": [row["edge_id"] for row in sorted(edges, key=lambda row: row["nearest_operating_surface_distance_m"]["maximum"], reverse=True)[:10]],
            "unconstrained": True,
        },
    }
    output_dir.mkdir(parents=True, exist_ok=True)
    (output_dir / "geometry-reconciliation.json").write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    _make_overlay(airport, output_dir / "geometry-reconciliation-overlay.png", used)
    return report


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--geometry", type=Path, default=DEFAULT_GEOMETRY)
    parser.add_argument("--scenario", type=Path, default=DEFAULT_SCENARIO)
    parser.add_argument("--events", type=Path, default=DEFAULT_EVENTS)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--samples", type=int, default=101)
    args = parser.parse_args()
    print(json.dumps(audit(args.geometry, args.scenario, args.events, args.output, args.samples), indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
