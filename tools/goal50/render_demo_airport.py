#!/usr/bin/env python3
"""Render a deterministic dimensioned plan view from the Demo Airport YAML."""

from __future__ import annotations

import argparse
import html
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "scenarios" / "ramplab_demo_airport.yaml"
DEFAULT_OUTPUT = ROOT / "evidence" / "goal50" / "airport-topology.svg"


def render(source: Path, output: Path) -> None:
    definition = yaml.safe_load(source.read_text(encoding="utf-8"))
    airport = definition["airport"]
    design = airport["design"]
    nodes = {node["id"]: node for node in airport["nodes"]}
    coordinates = [(float(node["x_m"]), float(node["y_m"])) for node in nodes.values()]
    for region in design["regions"]:
        coordinates.extend((float(point["x_m"]), float(point["y_m"])) for point in region["footprint"])
    for stand in design["stands"]:
        coordinates.extend((float(point["x_m"]), float(point["y_m"])) for point in stand["footprint"])
    min_x = min(point[0] for point in coordinates)
    max_x = max(point[0] for point in coordinates)
    min_y = min(point[1] for point in coordinates)
    max_y = max(point[1] for point in coordinates)
    scale = min(1000 / (max_x - min_x), 660 / (max_y - min_y))
    offset_x = 60 + (1000 - (max_x - min_x) * scale) / 2
    offset_y = 105 + (660 - (max_y - min_y) * scale) / 2

    def xy(x: float, y: float) -> tuple[float, float]:
        return offset_x + (x - min_x) * scale, 105 + offset_y + (max_y - y) * scale

    def point(node_id: str) -> tuple[float, float]:
        node = nodes[node_id]
        return xy(float(node["x_m"]), float(node["y_m"]))

    parts = [
        '<svg xmlns="http://www.w3.org/2000/svg" width="1120" height="850" viewBox="0 0 1120 850">',
        '<rect width="1120" height="850" fill="#f4f6f8"/>',
        '<text x="50" y="42" font-family="Segoe UI, sans-serif" font-size="25" font-weight="700" fill="#182b3a">RampLab Demo Airport</text>',
        '<text x="50" y="68" font-family="Segoe UI, sans-serif" font-size="14" fill="#536675">Dimensioned local-meter plan view • fictional • +X east / +Y north</text>',
    ]
    region_colors = {"runway": "#4b5563", "taxiway": "#718096", "apron": "#d9d5c8", "service_road": "#9ac2a7"}
    for region in design["regions"]:
        polygon = " ".join(f"{x:.1f},{y:.1f}" for x, y in
                           (xy(float(p["x_m"]), float(p["y_m"])) for p in region["footprint"]))
        color = region_colors.get(region["kind"], "#d1d5db")
        parts.append(f'<polygon points="{polygon}" fill="{color}" stroke="#59636e" stroke-width="1.2" opacity="0.9"/>')
        if region["kind"] == "runway":
            parts.append(f'<text x="{xy(0, 0)[0]:.1f}" y="{xy(0, 0)[1]-5:.1f}" text-anchor="middle" font-family="Segoe UI, sans-serif" font-size="13" fill="#fff">09/27 · 1,400 × 30 m</text>')

    for area in design["support_areas"]:
        polygon = " ".join(f"{x:.1f},{y:.1f}" for x, y in
                           (xy(float(p["x_m"]), float(p["y_m"])) for p in area["footprint"]))
        parts.append(f'<polygon points="{polygon}" fill="#f6e7a8" stroke="#a7791f" stroke-width="1.5"/>')

    for stand in design["stands"]:
        polygon = " ".join(f"{x:.1f},{y:.1f}" for x, y in
                           (xy(float(p["x_m"]), float(p["y_m"])) for p in stand["footprint"]))
        parts.append(f'<polygon points="{polygon}" fill="#c4b5fd" stroke="#6b46c1" stroke-width="1.5"/>')
        sx, sy = point(stand["node"])
        parts.append(f'<text x="{sx:.1f}" y="{sy+4:.1f}" text-anchor="middle" font-family="Segoe UI, sans-serif" font-size="11" font-weight="700" fill="#35215f">{html.escape(stand["id"])}</text>')

    for edge in airport["edges"]:
        x1, y1 = point(edge["from"])
        x2, y2 = point(edge["to"])
        service_only = not edge.get("aircraft_allowed", True)
        color = "#23824e" if service_only else "#d7e6f2"
        width = max(1.5, min(12, float(edge["width_m"]) * scale))
        parts.append(f'<line x1="{x1:.1f}" y1="{y1:.1f}" x2="{x2:.1f}" y2="{y2:.1f}" stroke="{color}" stroke-width="{width:.1f}" stroke-linecap="round" opacity="0.9"/>')

    for runway in design["runways"]:
        for designation, key in (("09", "threshold_09"), ("27", "threshold_27")):
            x, y = xy(float(runway[key]["x_m"]), float(runway[key]["y_m"]))
            parts.append(f'<circle cx="{x:.1f}" cy="{y:.1f}" r="4" fill="#fff" stroke="#111827" stroke-width="2"/><text x="{x:.1f}" y="{y-9:.1f}" text-anchor="middle" font-family="Segoe UI, sans-serif" font-size="12" font-weight="700" fill="#111827">{designation}</text>')

    for node_id, node in nodes.items():
        x, y = point(node_id)
        fill = "#c53030" if node_id.startswith("exit_") else "#17324d"
        if node_id == "hold_09":
            fill = "#f59e0b"
        radius = 4 if node_id not in {"hold_09", "depot"} else 6
        parts.append(f'<circle cx="{x:.1f}" cy="{y:.1f}" r="{radius}" fill="{fill}" stroke="#fff" stroke-width="1.2"/>')
        if node_id in {"hold_09", "depot", "terminal", "maintenance", "fuel_depot", "baggage_depot", "exit_-250", "exit_250"}:
            label = html.escape(node_id.replace("_", " "))
            parts.append(f'<text x="{x+7:.1f}" y="{y-6:.1f}" font-family="Segoe UI, sans-serif" font-size="10" fill="#263746">{label}</text>')

    parts.extend([
        '<rect x="42" y="785" width="1036" height="42" rx="7" fill="#fff" stroke="#ccd4dc"/>',
        '<text x="58" y="811" font-family="Segoe UI, sans-serif" font-size="12" fill="#263746">Runway / taxiway / apron / service-road and support-area footprints are authored geometry. The engine models runway occupancy as a timed resource, not a pavement route.</text>',
        '</svg>',
    ])
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("\n".join(parts) + "\n", encoding="utf-8", newline="\n")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=SOURCE)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()
    render(args.source, args.output)


if __name__ == "__main__":
    main()
