#!/usr/bin/env python3
"""Deterministically author the fully dimensioned fictional Demo Airport map."""

from __future__ import annotations

import math
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[2]
SCENARIO = ROOT / "scenarios" / "ramplab_demo_airport.yaml"


def rect(cx: float, cy: float, length: float, width: float) -> list[list[float]]:
    return [{"x_m": x, "y_m": y} for x, y in
            [(cx-length/2, cy-width/2), (cx+length/2, cy-width/2),
             (cx+length/2, cy+width/2), (cx-length/2, cy+width/2)]]


def main() -> None:
    doc = yaml.safe_load(SCENARIO.read_text(encoding="utf-8"))
    old = doc["airport"]
    nodes: list[dict] = []
    node_xy: dict[str, tuple[float, float]] = {}

    def node(ident: str, name: str, x: float, y: float) -> None:
        nodes.append({"id": ident, "name": name, "x_m": x, "y_m": y})
        node_xy[ident] = (x, y)

    node("origin", "Local coordinate origin", 0, 0)
    taxi_x = sorted(set([-600, -500, -250, 0, 250, 500, 600] + [-400, -200, 200, 400]))
    for x in taxi_x:
        node("hold_09" if x == -500 else f"taxi_{x}",
             "Runway 09 departure hold" if x == -500 else f"Taxiway A {x:+} m", x, 100)
    for x in [-250, 250]:
        node(f"exit_{x}", f"Runway arrival exit {x:+}", x, 0)
    gate_x = [-400, -200, 0, 200, 400]
    gates = ["A1", "A2", "A3", "A4", "A5"]
    for gate, x in zip(gates, gate_x):
        node(f"gate_{gate.lower()}", f"Stand {gate} center", x, 270)
        node(f"service_{gate.lower()}", f"Stand {gate} service spur", x, 350)
    service_x = [-600, -400, -200, 0, 200, 400, 600]
    road_id = lambda x: "depot" if x == -600 else f"road_{x}"
    for x in service_x:
        node(road_id(x),
             "GSE depot" if x == -600 else f"GSE road {x:+} m", x, 430)
    node("maintenance", "Maintenance and overflow", 600, 520)
    node("terminal", "Terminal apron reference", 0, 500)
    node("fuel_depot", "Fuel vehicle staging bay", -650, 390)
    node("baggage_depot", "Baggage vehicle staging bay", -650, 470)

    edges: list[dict] = []

    def edge(ident: str, a: str, b: str, *, aircraft: bool, gse: bool,
             surface: str, width: float, clearance: float = 0, runway: bool = False,
             one_way: bool = False) -> None:
        ax, ay = node_xy[a]
        bx, by = node_xy[b]
        distance = math.hypot(bx-ax, by-ay)
        edges.append({"id": ident, "from": a, "to": b, "distance_m": round(distance, 3),
                      "traversal_time_seconds": max(1, math.ceil(distance / 4)),
                      "surface_centerline": [{"x_m": ax, "y_m": ay}, {"x_m": bx, "y_m": by}], "aircraft_allowed": aircraft,
                      "ground_vehicle_allowed": gse, "surface_route_crosses_runway": runway,
                      "surface_type": surface, "width_m": width,
                      "minimum_clearance_m": clearance, "one_way": one_way})

    def gse_link(ident: str, a: str, b: str, surface: str, width: float = 7,
                 clearance: float = 4) -> None:
        edge(f"{ident}_out", a, b, aircraft=False, gse=True, surface=surface,
             width=width, clearance=clearance, one_way=True)
        edge(f"{ident}_back", b, a, aircraft=False, gse=True, surface=surface,
             width=width, clearance=clearance, one_way=True)

    taxi_nodes = ["hold_09" if x == -500 else f"taxi_{x}" for x in taxi_x]
    for i, (a, b) in enumerate(zip(taxi_nodes, taxi_nodes[1:])):
        edge(f"taxi_a_{i}", a, b, aircraft=True, gse=False, surface="taxiway_asphalt", width=18, clearance=15)
    for gate, x in zip(gates, gate_x):
        edge(f"taxi_stand_{gate.lower()}", "hold_09" if x == -500 else f"taxi_{x}", f"gate_{gate.lower()}",
             aircraft=True, gse=False, surface="taxiway_asphalt", width=18, clearance=15)
    for x in [-250, 250]:
        edge(f"exit_connector_{x}", f"exit_{x}", f"taxi_{x}", aircraft=True, gse=False,
             surface="runway_exit_asphalt", width=18, clearance=15)
    for a, b in zip(service_x, service_x[1:]):
        gse_link(f"gse_road_{a}_{b}", road_id(a), road_id(b), "service_road_paved")
    for gate, x in zip(gates, gate_x):
        gse_link(f"service_spur_{gate.lower()}", f"gate_{gate.lower()}", f"service_{gate.lower()}", "apron_service_spur")
        gse_link(f"service_road_link_{gate.lower()}", f"service_{gate.lower()}", f"road_{x}", "service_road_paved")
    gse_link("maintenance_link", "road_600", "maintenance", "service_road_paved")
    gse_link("terminal_service_link", "road_0", "terminal", "service_road_paved")
    gse_link("fuel_depot_link", "depot", "fuel_depot", "service_road_paved")
    gse_link("baggage_depot_link", "depot", "baggage_depot", "service_road_paved")

    stand_records = []
    for gate, x in zip(gates, gate_x):
        stand_records.append({"id": gate, "gate": gate, "node": f"gate_{gate.lower()}",
            "footprint": rect(x, 270, 52, 45), "length_m": 52, "width_m": 45,
            "maximum_wingspan_m": 36, "wingtip_clearance_m": 8,
            "pushback_direction": "north", "tug_required": False,
            "aircraft_access_edge": f"taxi_stand_{gate.lower()}",
            "service_access_edge": f"service_spur_{gate.lower()}_out"})
    design = {
        "airport_name": "RampLab Demo Airport", "fictional": True,
        "coordinate_frame": {"units": "meters", "origin_node": "origin", "x_axis": "east", "y_axis": "north", "z_axis": "up"},
        "unreal_centimeters_per_meter": 100,
        "runways": [{"id": "RWY09_27", "designation": "09/27", "threshold_09": {"x_m": -700, "y_m": 0},
            "threshold_27": {"x_m": 700, "y_m": 0}, "length_m": 1400, "width_m": 30,
            "surface_type": "asphalt", "departure_hold_node": "hold_09",
            "arrival_exits": [{"id": "ECHO", "node": "exit_-250", "connector_edge": "exit_connector_-250"},
                              {"id": "FOXTROT", "node": "exit_250", "connector_edge": "exit_connector_250"}]}],
        "hold_points": [{"id": "HOLD_09", "node": "hold_09", "protected_clearance_m": 15}],
        "stands": stand_records,
        "support_areas": [
            {"id": "GSE_DEPOT", "kind": "gse_depot", "node": "depot", "footprint": rect(-600, 430, 90, 60)},
            {"id": "FUEL_STAGING", "kind": "fuel_staging", "node": "road_600", "footprint": rect(600, 430, 80, 50)},
            {"id": "MAINTENANCE", "kind": "maintenance_overflow", "node": "maintenance", "footprint": rect(600, 520, 100, 70)},
            {"id": "TERMINAL", "kind": "terminal_apron", "node": "terminal", "footprint": rect(0, 500, 240, 90)}],
        "regions": [
            {"id": "RUNWAY_PAVEMENT", "kind": "runway", "surface_type": "asphalt", "elevation_m": 0, "footprint": rect(0, 0, 1400, 30)},
            {"id": "TAXIWAY_A", "kind": "taxiway", "surface_type": "asphalt", "elevation_m": 0, "footprint": rect(0, 100, 1200, 18)},
            {"id": "APRON", "kind": "apron", "surface_type": "concrete", "elevation_m": 0, "footprint": [{"x_m": x, "y_m": y} for x, y in [(-500, 180), (500, 180), (500, 320), (-500, 320)]]},
            {"id": "SERVICE_ROAD", "kind": "service_road", "surface_type": "paved", "elevation_m": 0, "footprint": rect(0, 430, 1200, 7)}]}
    old["design"] = design
    old["nodes"] = nodes
    old["edges"] = edges
    doc["gates"] = [{"id": gate, "node": f"gate_{gate.lower()}"} for gate in gates]
    for vehicle in doc["fleet"]["vehicles"]:
        vehicle["depot_node"] = "fuel_depot" if vehicle["id"].startswith("fuel") else "baggage_depot"
    for flight in doc["aircraft"]:
        flight["gate"] = flight["gate"]
        for task in flight.get("service_tasks", []):
            if task["id"] == "baggage_unload":
                task["prerequisites"] = ["deboard", "fuel"]
        if flight.get("operation_type") in {"arrival_turnaround", "arrival_only"}:
            flight["arrival_exit"] = "exit_-250" if flight["gate"] == "A4" else "exit_250"
    doc["surface_operations"].update({"departure_handoff": "hold_09", "runway_node": "hold_09",
                                      "arrival_exit": "exit_-250", "aircraft_speed_mps": 4.0,
                                      "return_service_vehicles_to_depot_after_task": True})
    SCENARIO.write_text(yaml.safe_dump(doc, sort_keys=False, width=110), encoding="utf-8")


if __name__ == "__main__":
    main()
