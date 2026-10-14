"""Audit Goal44 KAUO route feasibility and exact GSE clearance."""
from __future__ import annotations

import heapq
import json
import math
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_SCENARIO = ROOT / "results/goal44/generated/disruption/scenario.json"
DEFAULT_OUTPUT = ROOT / "results/goal44/topology-diagnostic.json"
CLEARANCE_M = 30.0


def _point_segment_distance(point: tuple[float, float], left: dict[str, float], right: dict[str, float]) -> float:
    dx, dy = right["x_m"] - left["x_m"], right["y_m"] - left["y_m"]
    denominator = dx * dx + dy * dy
    fraction = 0.0 if denominator == 0 else max(0.0, min(1.0,
        ((point[0] - left["x_m"]) * dx + (point[1] - left["y_m"]) * dy) / denominator))
    return math.hypot(point[0] - left["x_m"] - fraction * dx,
                      point[1] - left["y_m"] - fraction * dy)


def _line_distance(point: tuple[float, float], line: list[dict[str, float]]) -> float:
    return min((_point_segment_distance(point, left, right)
                for left, right in zip(line, line[1:])), default=math.inf)


def _shortest_path(edges: list[dict[str, Any]], start: str, finish: str,
                   excluded_edges: set[str] | None = None) -> dict[str, Any] | None:
    adjacency: dict[str, list[tuple[str, dict[str, Any]]]] = {}
    excluded_edges = excluded_edges or set()
    for edge in edges:
        if not edge.get("aircraft_allowed") or edge["id"] in excluded_edges:
            continue
        adjacency.setdefault(edge["from"], []).append((edge["to"], edge))
        adjacency.setdefault(edge["to"], []).append((edge["from"], edge))
    queue: list[tuple[float, str, tuple[str, ...]]] = [(0.0, start, ())]
    best: dict[str, float] = {}
    while queue:
        cost, node, path = heapq.heappop(queue)
        if node in best:
            continue
        best[node] = cost
        if node == finish:
            return {"reachable": True, "distance_m": round(cost, 3), "edge_ids": list(path)}
        for neighbor, edge in adjacency.get(node, []):
            if neighbor not in best:
                heapq.heappush(queue, (cost + float(edge["surface_route_distance_m"]), neighbor,
                                       path + (edge["id"],)))
    return None


def audit(scenario_path: Path = DEFAULT_SCENARIO) -> dict[str, Any]:
    scenario = json.loads(scenario_path.read_text(encoding="utf-8-sig"))
    airport = scenario["airport"]
    nodes = {node["id"]: node for node in airport["nodes"]}
    edges = airport["edges"]
    depots = [(node["id"], (float(node["x_m"]), float(node["y_m"])))
              for node in nodes.values()
              if str(node.get("classification", "")).startswith("APPROXIMATE GSE depot")]
    attachments = [node["surface_attachment"] | {"node_id": node["id"]}
                   for node in nodes.values() if isinstance(node.get("surface_attachment"), dict)]
    attachment_displacements = [float(item["attachment_displacement_m"]) for item in attachments]
    unverified_endpoints = [item for item in attachments
                            if item.get("classification") == "UNVERIFIED_APPROXIMATE_NODE_SNAP"]
    clearance_rows = []
    for edge in edges:
        if not edge.get("aircraft_allowed"):
            continue
        closest = min((( _line_distance(point, edge["surface_centerline"]), node_id)
                       for node_id, point in depots), default=(math.inf, None))
        clearance_rows.append({"edge_id": edge["id"], "minimum_clearance_m": round(closest[0], 3),
                               "nearest_depot_node": closest[1],
                               "passes_30m_clearance": closest[0] >= CLEARANCE_M})
    gates = {gate["id"]: gate["node"] for gate in scenario["gates"]}
    disruption = scenario.get("road_events", [])
    closed_edges = {event["edge"] for event in disruption if event.get("enabled") is False}
    edge_by_id = {edge["id"]: edge for edge in edges}
    flights = []
    for aircraft in scenario["aircraft"]:
        gate_node = gates[aircraft["gate"]]
        flights.append({"aircraft_id": aircraft["id"], "arrival_exit": aircraft["arrival_exit"],
                        "gate_id": aircraft["gate"], "gate_node": gate_node,
                        "arrival_to_gate": _shortest_path(edges, aircraft["arrival_exit"], gate_node),
                        "gate_to_departure_handoff": _shortest_path(edges, gate_node, "rwy36_hold"),
                        "arrival_to_gate_while_scheduled_edges_closed": _shortest_path(
                            edges, aircraft["arrival_exit"], gate_node, closed_edges)})
        closure_path = flights[-1]["arrival_to_gate_while_scheduled_edges_closed"]
        if closure_path:
            closure_path["crosses_runway_inventory"] = any(
                edge_by_id[edge_id].get("surface_route_crosses_runway", False)
                for edge_id in closure_path["edge_ids"])
            closure_path["contains_unverified_runway_crossing"] = any(
                edge_by_id[edge_id].get("ground_vehicle_access_limitation")
                for edge_id in closure_path["edge_ids"])
    return {
        "classification": "DERIVED_SIMULATION_TOPOLOGY_AUDIT",
        "scenario": str(scenario_path.relative_to(ROOT)) if scenario_path.is_relative_to(ROOT) else scenario_path.name,
        "clearance_requirement_m": CLEARANCE_M,
        "aircraft_allowed_edge_count": len(clearance_rows),
        "ground_only_edge_count": sum(not edge.get("aircraft_allowed", False) for edge in edges),
        "node_attachment_summary": {
            "node_count": len(attachments),
            "unverified_approximate_node_snap_count": len(unverified_endpoints),
            "maximum_displacement_m": round(max(attachment_displacements, default=0.0), 3),
            "mean_displacement_m": round(sum(attachment_displacements) / len(attachment_displacements), 3)
                if attachment_displacements else 0.0,
            "largest_displacements": sorted(
                ({"node_id": item["node_id"], "displacement_m": item["attachment_displacement_m"]}
                 for item in unverified_endpoints),
                key=lambda item: (-item["displacement_m"], item["node_id"]))[:10],
        },
        "minimum_aircraft_edge_clearance_m": round(min((row["minimum_clearance_m"] for row in clearance_rows), default=math.inf), 3),
        "clearance_pass": all(row["passes_30m_clearance"] for row in clearance_rows),
        "edge_clearance": clearance_rows,
        "flight_routes": flights,
        "all_arrivals_and_departures_reachable": all(
            row["arrival_to_gate"] and row["gate_to_departure_handoff"] for row in flights),
        "scheduled_closed_edges": sorted(closed_edges),
        "all_arrivals_reachable_in_static_graph_during_scheduled_closure": all(
            row["arrival_to_gate_while_scheduled_edges_closed"] for row in flights) if closed_edges else None,
        "limitations": [
            "Route geometry derives from ALDOT pavement inventory polygons, not approved taxiway centerlines.",
            "Stand and GSE depot positions remain explicitly approximate simulation assumptions.",
            "Large node snaps are unverified endpoint approximations; a pavement attachment does not establish the real location of an assumed stand or service point.",
            "Graph reachability does not establish legal runway crossing, approved airport movement, or live operational suitability.",
        ],
    }


if __name__ == "__main__":
    result = audit()
    DEFAULT_OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    DEFAULT_OUTPUT.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps({"output": str(DEFAULT_OUTPUT), "clearance_pass": result["clearance_pass"],
                      "routes_pass": result["all_arrivals_and_departures_reachable"],
                      "minimum_clearance_m": result["minimum_aircraft_edge_clearance_m"]}, sort_keys=True))
    if not result["clearance_pass"] or not result["all_arrivals_and_departures_reachable"]:
        raise SystemExit(1)
