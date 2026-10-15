"""Render a review image of the derived KAUO topology and GSE clearance areas."""
from __future__ import annotations

import json
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[2]
GEOMETRY = ROOT / "tools/airport_scenario_generation/kauo_goal44.geometry.json"
OUTPUT = ROOT / "results/goal44/topology.png"


def render() -> Path:
    airport = json.loads(GEOMETRY.read_text(encoding="utf-8-sig"))["airport"]
    nodes = {node["id"]: node for node in airport["nodes"]}
    points = [point for area in airport["surface_areas"] for polygon in area["polygons"]
              for ring in polygon for point in ring]
    min_x, max_x = min(p["x_m"] for p in points), max(p["x_m"] for p in points)
    min_y, max_y = min(p["y_m"] for p in points), max(p["y_m"] for p in points)
    width, height, margin = 1600, 1100, 70
    scale = min((width - margin * 2) / (max_x - min_x), (height - margin * 2) / (max_y - min_y))

    def xy(point: dict[str, float]) -> tuple[int, int]:
        return (round(margin + (point["x_m"] - min_x) * scale),
                round(height - margin - (point["y_m"] - min_y) * scale))

    image = Image.new("RGB", (width, height), "#f6f8fb")
    draw = ImageDraw.Draw(image)
    colors = {"RUNWAY": "#dce2e9", "TAXIWAY": "#e8edf2", "APRON": "#f0e8d7"}
    for area in airport["surface_areas"]:
        color = colors.get(area["use"])
        if not color:
            continue
        for polygon in area["polygons"]:
            draw.polygon([xy(point) for point in polygon[0]], fill=color, outline="#bac4cf")
    for edge in airport["edges"]:
        if not edge.get("aircraft_allowed"):
            continue
        line = [xy(point) for point in edge["surface_centerline"]]
        draw.line(line, fill="#1769aa", width=5, joint="curve")
    for node in nodes.values():
        if str(node.get("classification", "")).startswith("APPROXIMATE GSE depot"):
            center = xy(node)
            radius = round(30 * scale)
            draw.ellipse((center[0] - radius, center[1] - radius,
                          center[0] + radius, center[1] + radius), outline="#d97706", width=3)
            draw.ellipse((center[0] - 6, center[1] - 6, center[0] + 6, center[1] + 6), fill="#b45309")
    labels = {"twy_a_mid": "Arrival exit", "rwy36_hold": "Departure handoff",
              "ga_parking_stand": "Gate 1", "ga_parking_stand_02": "Gate 2",
              "ga_parking_stand_03": "Gate 3"}
    for node_id in labels:
        if node_id in nodes:
            center = xy(nodes[node_id])
            draw.ellipse((center[0] - 5, center[1] - 5, center[0] + 5, center[1] + 5), fill="#14324a")
            offset = {"ga_parking_stand": (10, 8), "ga_parking_stand_02": (10, -26),
                      "ga_parking_stand_03": (10, -8)}.get(node_id, (8, -15))
            draw.text((center[0] + offset[0], center[1] + offset[1]), labels[node_id], fill="#14324a")
    draw.rectangle((18, 18, width - 18, 80), fill="#ffffff", outline="#d5dde5")
    draw.text((34, 30), "KAUO Goal44 derived movement topology", fill="#14283b")
    draw.text((34, 55), "Blue: aircraft routes  |  Orange rings: 30 m around nine assumed GSE depots",
              fill="#40576b")
    draw.text((34, height - 38), "ALDOT pavement inventory based; approximate stands/GSE; not approved taxiway centerlines",
              fill="#40576b")
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    image.save(OUTPUT, optimize=True)
    return OUTPUT


if __name__ == "__main__":
    print(render())
