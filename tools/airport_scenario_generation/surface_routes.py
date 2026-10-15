"""Derive deterministic movement paths constrained to source pavement polygons.

ALDOT pavement sections provide areas, not centerlines. This module derives a
repeatable interior path through those areas and retains a per-node attachment
record so approximate input locations are never mistaken for surveyed points.
"""
from __future__ import annotations

import heapq
import math
from typing import Any

from PIL import Image, ImageChops, ImageDraw, ImageFilter


GRID_M = 2.0
EDGE_INSET_CELLS = 1
GSE_DEPOT_MIN_SEPARATION_M = 8.0
# Two 0.75 m vehicle radii plus the fleet's 1.25 m physical margin is 2.75 m;
# the 2 m raster's next representable clear spacing is 4 m.
GSE_SERVICE_POINT_MIN_SEPARATION_M = 4.0
GSE_SERVICE_APPROACH_MIN_SEPARATION_M = 6.0
AIRCRAFT_USES = {"RUNWAY", "TAXIWAY", "APRON"}
GROUND_USES = {"TAXIWAY", "APRON"}
RUNWAY_NODE_IDS = {
    "rwy18_36_mid", "rwy36_hold", "rwy18_threshold", "rwy11_threshold",
    "rwy29_threshold", "runway_intersection",
}


def _point_in_ring(x: float, y: float, ring: list[dict[str, float]]) -> bool:
    inside = False
    for left, right in zip(ring, ring[1:]):
        x1, y1 = left["x_m"], left["y_m"]
        x2, y2 = right["x_m"], right["y_m"]
        if (y1 > y) != (y2 > y) and x < (x2 - x1) * (y - y1) / (y2 - y1) + x1:
            inside = not inside
    return inside


class _SurfaceGrid:
    def __init__(self, areas: list[dict[str, Any]], uses: set[str],
                 blocked_points: list[tuple[float, float]] | None = None,
                 blocked_radius_m: float = 0.0):
        polygons = [polygon for area in areas if area["use"] in uses for polygon in area["polygons"]]
        if not polygons:
            raise ValueError(f"no pavement polygons found for movement classes using {sorted(uses)}")
        points = [point for polygon in polygons for ring in polygon for point in ring]
        margin = GRID_M * 4
        self.x0 = math.floor(min(point["x_m"] for point in points) / GRID_M) * GRID_M - margin
        self.y1 = math.ceil(max(point["y_m"] for point in points) / GRID_M) * GRID_M + margin
        self.width = math.ceil((max(point["x_m"] for point in points) - self.x0 + margin) / GRID_M) + 1
        self.height = math.ceil((self.y1 - min(point["y_m"] for point in points) + margin) / GRID_M) + 1
        combined = Image.new("L", (self.width, self.height), 0)
        for polygon in polygons:
            part = Image.new("L", (self.width, self.height), 0)
            draw = ImageDraw.Draw(part)
            draw.polygon([self.world_to_pixel(point) for point in polygon[0]], fill=255)
            for hole in polygon[1:]:
                draw.polygon([self.world_to_pixel(point) for point in hole], fill=0)
            combined = ImageChops.lighter(combined, part)
        if EDGE_INSET_CELLS:
            # A 2 m raster inset keeps route samples away from inventory edges;
            # it is not a centerline or vehicle-clearance guarantee.
            combined = combined.filter(ImageFilter.MinFilter(EDGE_INSET_CELLS * 2 + 1))
        if blocked_points and blocked_radius_m > 0:
            draw = ImageDraw.Draw(combined)
            radius = math.ceil(blocked_radius_m / GRID_M)
            for point in blocked_points:
                col, row = self.world_to_pixel(point)
                draw.ellipse((col - radius, row - radius, col + radius, row + radius), fill=0)
        self.cells = combined.tobytes()
        self.mask = combined
        self.uses = uses

    def world_to_pixel(self, point: dict[str, float] | tuple[float, float]) -> tuple[int, int]:
        x = point["x_m"] if isinstance(point, dict) else point[0]
        y = point["y_m"] if isinstance(point, dict) else point[1]
        return (round((x - self.x0) / GRID_M), self.height - 1 - round((self.y1 - y) / GRID_M))

    def pixel_to_world(self, point: tuple[int, int]) -> tuple[float, float]:
        col, row = point
        return (self.x0 + col * GRID_M, self.y1 - (self.height - 1 - row) * GRID_M)

    def open(self, point: tuple[int, int]) -> bool:
        col, row = point
        return 0 <= col < self.width and 0 <= row < self.height and self.cells[row * self.width + col] != 0

    def nearest(self, point: tuple[float, float], maximum_distance_m: float) -> tuple[int, int] | None:
        origin = self.world_to_pixel(point)
        if self.open(origin):
            return origin
        maximum_cells = math.ceil(maximum_distance_m / GRID_M)
        for radius in range(1, maximum_cells + 1):
            candidates: list[tuple[int, int]] = []
            for dx in range(-radius, radius + 1):
                for dy in (-radius, radius):
                    candidate = origin[0] + dx, origin[1] + dy
                    if self.open(candidate):
                        candidates.append(candidate)
            for dy in range(-radius + 1, radius):
                for dx in (-radius, radius):
                    candidate = origin[0] + dx, origin[1] + dy
                    if self.open(candidate):
                        candidates.append(candidate)
            if candidates:
                return min(candidates, key=lambda item: (
                    (item[0] - origin[0]) ** 2 + (item[1] - origin[1]) ** 2,
                    item[1], item[0]))
        return None

    def path(self, start: tuple[int, int], finish: tuple[int, int]) -> list[tuple[int, int]] | None:
        if start == finish:
            return [start]
        neighbors = ((-1, 0, 1.0), (1, 0, 1.0), (0, -1, 1.0), (0, 1, 1.0),
                     (-1, -1, math.sqrt(2)), (-1, 1, math.sqrt(2)),
                     (1, -1, math.sqrt(2)), (1, 1, math.sqrt(2)))
        frontier = [(math.dist(start, finish), 0.0, start)]
        previous: dict[tuple[int, int], tuple[int, int]] = {}
        costs = {start: 0.0}
        while frontier:
            _, current_cost, current = heapq.heappop(frontier)
            if current_cost != costs.get(current):
                continue
            if current == finish:
                result = [current]
                while current in previous:
                    current = previous[current]
                    result.append(current)
                return list(reversed(result))
            for dx, dy, step_cost in neighbors:
                nxt = current[0] + dx, current[1] + dy
                if not self.open(nxt):
                    continue
                if dx and dy and (not self.open((current[0] + dx, current[1])) or
                                  not self.open((current[0], current[1] + dy))):
                    continue
                candidate_cost = current_cost + step_cost
                if candidate_cost >= costs.get(nxt, math.inf):
                    continue
                costs[nxt] = candidate_cost
                previous[nxt] = current
                heapq.heappush(frontier, (candidate_cost + math.dist(nxt, finish), candidate_cost, nxt))
        return None

    def path_points(self, path: list[tuple[int, int]]) -> list[dict[str, float]]:
        # Keep each raster direction change. These are reproducible route
        # vertices, not surveyed taxiway centerline observations.
        kept = [path[0]]
        previous_direction = None
        for left, right in zip(path, path[1:]):
            direction = right[0] - left[0], right[1] - left[1]
            if previous_direction is not None and direction != previous_direction:
                kept.append(left)
            previous_direction = direction
        if path[-1] != kept[-1]:
            kept.append(path[-1])
        return [{"x_m": round(x, 3), "y_m": round(y, 3)} for x, y in map(self.pixel_to_world, kept)]


def _nearest_area(point: tuple[float, float], areas: list[dict[str, Any]], uses: set[str]) -> tuple[float, str | None]:
    nearest = (math.inf, None)
    for area in areas:
        if area["use"] not in uses:
            continue
        for polygon in area["polygons"]:
            if _point_in_ring(*point, polygon[0]) and not any(_point_in_ring(*point, hole) for hole in polygon[1:]):
                return 0.0, area["id"]
            for ring in polygon:
                for left, right in zip(ring, ring[1:]):
                    dx, dy = right["x_m"] - left["x_m"], right["y_m"] - left["y_m"]
                    length2 = dx * dx + dy * dy
                    t = 0.0 if length2 == 0 else max(0.0, min(1.0,
                        ((point[0] - left["x_m"]) * dx + (point[1] - left["y_m"]) * dy) / length2))
                    distance = math.hypot(point[0] - left["x_m"] - t * dx,
                                          point[1] - left["y_m"] - t * dy)
                    if distance < nearest[0]:
                        nearest = distance, area["id"]
    return nearest


def derive_surface_routes(airport: dict[str, Any], maximum_attachment_distance_m: float = 250.0,
                          aircraft_clearance_m: float = 0.0,
                          explicit_aircraft_edges: bool = False,
                          goal44_endpoint_audit: bool = False) -> dict[str, Any]:
    """Attach approximate graph nodes and route edges to classified pavement.

    Ground vehicles use TAXIWAY/APRON only. Aircraft may also use RUNWAY.
    Edges lacking a complete path on a movement class are disabled for it.
    """
    areas = airport.get("surface_areas", [])
    aircraft_grid = _SurfaceGrid(areas, AIRCRAFT_USES)
    ground_grid = _SurfaceGrid(areas, GROUND_USES)
    nodes = {node["id"]: node for node in airport.get("nodes", [])}
    node_cells: dict[str, dict[str, tuple[int, int] | None]] = {key: {} for key in nodes}
    attachments = []
    claimed_cells: dict[str, set[tuple[int, int]]] = {"aircraft": set(), "ground_vehicle": set()}
    attached_gse_depots: list[tuple[float, float]] = []
    attached_gse_service_points: list[tuple[float, float]] = []
    service_approaches = []
    if goal44_endpoint_audit:
        for node_id, node in nodes.items():
            if not node_id.endswith("_service_approach"):
                continue
            approach_cell = ground_grid.nearest((float(node["x_m"]), float(node["y_m"])),
                                                maximum_attachment_distance_m)
            if approach_cell is not None:
                service_approaches.append(ground_grid.pixel_to_world(approach_cell))
    for node_id, node in nodes.items():
        original = (float(node["x_m"]), float(node["y_m"]))
        movement = "aircraft" if node_id in RUNWAY_NODE_IDS else "ground_vehicle"
        grid = aircraft_grid if movement == "aircraft" else ground_grid
        cell = grid.nearest(original, maximum_attachment_distance_m)
        if cell is None:
            raise ValueError(f"node {node_id!r} cannot attach to an eligible operating surface within {maximum_attachment_distance_m:g} m")
        is_gse_depot = node_id.startswith("gse_") and node_id.endswith("_depot")
        is_gse_service_point = goal44_endpoint_audit and node_id.endswith("_service_point")
        attached_candidate = grid.pixel_to_world(cell)
        depot_conflict = is_gse_depot and any(
            math.dist(attached_candidate, previous) < GSE_DEPOT_MIN_SEPARATION_M
            for previous in attached_gse_depots)
        service_point_conflict = is_gse_service_point and any(
            math.dist(attached_candidate, previous) < GSE_SERVICE_POINT_MIN_SEPARATION_M
            for previous in attached_gse_service_points)
        approach_conflict = is_gse_service_point and any(
            math.dist(attached_candidate, approach) < GSE_SERVICE_APPROACH_MIN_SEPARATION_M
            for approach in service_approaches)
        if ((cell in claimed_cells[movement] and math.dist(original, attached_candidate) > GRID_M / 2) or
                depot_conflict or service_point_conflict or approach_conflict):
            # The nearest pavement cell can be far from its inherited approximate
            # node. Search around that actual attachment, not the off-pavement
            # source point, so a collision cannot silently preserve a duplicate.
            anchor_cell = cell
            candidates = []
            for radius in range(1, 11):
                for dx in range(-radius, radius + 1):
                    for dy in range(-radius, radius + 1):
                        if max(abs(dx), abs(dy)) != radius:
                            continue
                        candidate = anchor_cell[0] + dx, anchor_cell[1] + dy
                        if candidate in claimed_cells[movement] or not grid.open(candidate):
                            continue
                        position = grid.pixel_to_world(candidate)
                        if is_gse_depot and any(math.dist(position, previous) < GSE_DEPOT_MIN_SEPARATION_M
                                                for previous in attached_gse_depots):
                            continue
                        if is_gse_service_point and any(
                                math.dist(position, previous) < GSE_SERVICE_POINT_MIN_SEPARATION_M
                                for previous in attached_gse_service_points):
                            continue
                        if is_gse_service_point and any(
                                math.dist(position, approach) < GSE_SERVICE_APPROACH_MIN_SEPARATION_M
                                for approach in service_approaches):
                            continue
                        candidates.append(candidate)
                if candidates:
                    cell = min(candidates, key=lambda item: (
                        math.dist(original, grid.pixel_to_world(item)), item[1], item[0]))
                    break
            else:
                if is_gse_depot:
                    raise ValueError(f"GSE depot node {node_id!r} cannot be attached at least "
                                     f"{GSE_DEPOT_MIN_SEPARATION_M:g} m from other depots on eligible pavement")
                if is_gse_service_point:
                    raise ValueError(f"GSE service point {node_id!r} cannot be attached at least "
                                     f"{GSE_SERVICE_POINT_MIN_SEPARATION_M:g} m from other service points on eligible pavement")
        claimed_cells[movement].add(cell)
        attached = grid.pixel_to_world(cell)
        nearest_distance, feature_id = _nearest_area(attached, areas, grid.uses)
        if feature_id is None or nearest_distance > GRID_M:
            raise ValueError(f"node {node_id!r} attachment is not supported by a classified pavement feature")
        displacement = math.dist(original, attached)
        node["x_m"], node["y_m"] = round(attached[0], 3), round(attached[1], 3)
        if is_gse_depot:
            attached_gse_depots.append(attached)
        if is_gse_service_point:
            attached_gse_service_points.append(attached)
        node_cells[node_id]["aircraft"] = cell if aircraft_grid.open(cell) else aircraft_grid.nearest(attached, GRID_M)
        node_cells[node_id]["ground_vehicle"] = cell if ground_grid.open(cell) else ground_grid.nearest(attached, GRID_M)
        node["surface_attachment"] = {
            "classification": ("SOURCE_BACKED_PAVEMENT_SNAP" if displacement <= GRID_M else "UNVERIFIED_APPROXIMATE_NODE_SNAP")
                if goal44_endpoint_audit else
                ("SOURCE_BACKED_DERIVED" if displacement <= GRID_M else "MANUALLY_RECONCILED_APPROXIMATION"),
            "source_feature_id": feature_id,
            "source": "ALDOT_AUO_PAVEMENT_GIS",
            "movement_surface": "RUNWAY_OR_TAXIWAY_OR_APRON" if node_id in RUNWAY_NODE_IDS else "TAXIWAY_OR_APRON",
            "original_position_local_m": {"x_m": round(original[0], 3), "y_m": round(original[1], 3)},
            "attachment_displacement_m": round(displacement, 3),
        }
        if goal44_endpoint_audit:
            node["surface_attachment"]["node_location_status"] = node.get("classification", "UNSPECIFIED")
        if aircraft_clearance_m > 0:
            node["surface_attachment"]["attached_position_local_m"] = {
                "x_m": round(attached[0], 3), "y_m": round(attached[1], 3)}
        attachments.append(node["surface_attachment"] | {"node_id": node_id})

    # Rebuild the aircraft raster after source-backed depot attachments so the
    # route generator preserves the current assumed staging positions and routes
    # around them. Clearance is measured from raster centerlines downstream.
    if aircraft_clearance_m > 0:
        depot_points = [tuple(node["surface_attachment"]["attached_position_local_m"][axis]
                              for axis in ("x_m", "y_m"))
                        for node in nodes.values()
                        if str(node.get("classification", "")).startswith("APPROXIMATE GSE depot")]
        aircraft_grid = _SurfaceGrid(areas, AIRCRAFT_USES, depot_points,
                                     aircraft_clearance_m + GRID_M * 2)
        for node_id, node in nodes.items():
            attached = (float(node["x_m"]), float(node["y_m"]))
            node_cells[node_id]["aircraft"] = aircraft_grid.nearest(attached, GRID_M)

    route_count = {"aircraft": 0, "ground_vehicle": 0, "unavailable_for_both": 0}
    if explicit_aircraft_edges:
        route_count["ground_only_by_semantics"] = 0
    for edge in airport.get("edges", []):
        if explicit_aircraft_edges:
            classification = edge.get("classification")
            if classification is None:
                aircraft_semantic = True
            elif classification == "APPROXIMATE connector to an illustrative parking position":
                aircraft_semantic = True
            elif classification.startswith(("ASSUMED FOR SIMULATION GSE", "ASSUMED FOR SIMULATION shared stand",
                                            "ASSUMED FOR SIMULATION short service", "APPROXIMATE GSE")):
                aircraft_semantic = False
            else:
                raise ValueError(f"edge {edge['id']!r} has an unrecognized movement classification: {classification!r}")
        else:
            aircraft_semantic = True
        route_options = []
        for movement, grid in (("aircraft", aircraft_grid), ("ground_vehicle", ground_grid)):
            start = node_cells[edge["from"]][movement]
            finish = node_cells[edge["to"]][movement]
            path = grid.path(start, finish) if start is not None and finish is not None else None
            route_options.append((movement, grid, path))
        preferred_movement = "aircraft" if aircraft_semantic and aircraft_clearance_m > 0 else "ground_vehicle"
        usable = next((item for item in route_options if item[0] == preferred_movement and item[2]), None)
        if usable is None:
            usable = next((item for item in route_options if item[2]), None)
        if usable is None:
            edge["aircraft_allowed"] = False
            edge["ground_vehicle_allowed"] = False
            edge["surface_centerline"] = []
            route_count["unavailable_for_both"] += 1
            raise ValueError(f"edge {edge['id']!r} has no connected classified-surface route")
        movement, grid, path = usable
        line = grid.path_points(path)
        distance = sum(math.hypot(right["x_m"] - left["x_m"], right["y_m"] - left["y_m"])
                       for left, right in zip(line, line[1:]))
        if distance <= 0:
            raise ValueError(f"edge {edge['id']!r} has a zero-length derived surface route")
        edge["surface_centerline"] = line
        edge["surface_route_distance_m"] = round(distance, 3)
        edge["aircraft_allowed"] = aircraft_semantic and route_options[0][2] is not None
        edge["ground_vehicle_allowed"] = route_options[1][2] is not None
        edge["surface_route_classification"] = "DERIVED_FROM_ALDOT_PAVEMENT_INTERIOR"
        if explicit_aircraft_edges:
            if not aircraft_semantic:
                edge["movement_class"] = "GROUND_VEHICLE_SERVICE_ONLY"
            elif edge["ground_vehicle_allowed"]:
                edge["movement_class"] = "AIRCRAFT_AND_GROUND_VEHICLE"
            else:
                edge["movement_class"] = "AIRCRAFT_ONLY; ground connection unavailable or unverified"
                edge["ground_vehicle_access_limitation"] = (
                    "No continuous TAXIWAY/APRON-only source-polygon route; runway-crossing permission is unverified")
        feature_ids = set()
        for pixel in path:
            feature_distance, feature_id = _nearest_area(grid.pixel_to_world(pixel), areas, grid.uses)
            if feature_id is not None and feature_distance <= GRID_M:
                feature_ids.add(feature_id)
        edge["surface_route_source_features"] = sorted(feature_ids)
        use_by_id = {area["id"]: area["use"] for area in areas}
        route_uses = sorted({use_by_id[feature_id] for feature_id in feature_ids if feature_id in use_by_id})
        edge["surface_route_uses"] = route_uses
        edge["surface_route_crosses_runway"] = "RUNWAY" in route_uses
        original_speed = float(edge["distance_m"]) / max(1, int(edge["traversal_time_seconds"]))
        edge["distance_m"] = round(distance, 3)
        edge["traversal_time_seconds"] = max(1, math.ceil(distance / original_speed))
        route_count["aircraft"] += int(edge["aircraft_allowed"])
        route_count["ground_vehicle"] += int(edge["ground_vehicle_allowed"])
        if explicit_aircraft_edges:
            route_count["ground_only_by_semantics"] += int(not aircraft_semantic)
    result = {
        "method": "2 m raster A* paths through classified ALDOT pavement interiors with a 2 m raster inset; 8-connected movement; diagonal corner cutting disallowed" +
            ("; approximate service-point attachments maintain at least 4 m separation" if goal44_endpoint_audit else ""),
        "grid_size_m": GRID_M,
        "source_uses": {"aircraft": sorted(AIRCRAFT_USES), "ground_vehicle": sorted(GROUND_USES)},
        "route_counts": route_count,
        "node_attachments": attachments,
        "classification": "SOURCE_BACKED_PAVEMENT_WITH_UNVERIFIED_APPROXIMATE_ENDPOINTS"
            if goal44_endpoint_audit else "SOURCE_BACKED_DERIVED_WITH_MANUALLY_RECONCILED_APPROXIMATIONS",
        **({"attachment_summary": {
            "node_count": len(attachments),
            "source_backed_pavement_snap_count": sum(
                item["classification"] == "SOURCE_BACKED_PAVEMENT_SNAP" for item in attachments),
            "gse_service_point_count": len(attached_gse_service_points),
            "gse_service_point_minimum_separation_m": round(min(
                (math.dist(left, right) for index, left in enumerate(attached_gse_service_points)
                 for right in attached_gse_service_points[index + 1:]), default=0.0), 3),
            "gse_service_point_required_separation_m": GSE_SERVICE_POINT_MIN_SEPARATION_M,
            "gse_service_approach_required_separation_m": GSE_SERVICE_APPROACH_MIN_SEPARATION_M,
            "unverified_approximate_node_snap_count": sum(
                item["classification"] == "UNVERIFIED_APPROXIMATE_NODE_SNAP" for item in attachments),
            "maximum_displacement_m": round(max(
                (item["attachment_displacement_m"] for item in attachments), default=0.0), 3),
            "mean_displacement_m": round(sum(
                item["attachment_displacement_m"] for item in attachments) / len(attachments), 3)
                if attachments else 0.0,
        }} if goal44_endpoint_audit else {}),
        "limitations": [
            "ALDOT pavement inventory polygons do not define operational centerlines, legal connectivity, markings, or current status.",
            "Raster routes are deterministic interior paths for simulation, not surveyed or approved airport routes.",
            "Approximate node displacements are recorded per node; current as-built apron geometry remains unverified.",
        ],
    }
    if goal44_endpoint_audit:
        result["limitations"].append(
            "GSE service-point snaps are illustrative attachment assumptions separated from shared service approaches and other points; they are not surveyed service-bay coordinates.")
    if aircraft_clearance_m > 0:
        result["aircraft_clearance_m"] = aircraft_clearance_m
        result["aircraft_clearance_source"] = "attached assumed GSE depot positions; path is generated with raster buffer and verified against exact polyline distance"
    return result
