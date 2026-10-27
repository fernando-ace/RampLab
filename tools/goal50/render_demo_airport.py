#!/usr/bin/env python3
"""Render a deterministic topology view from the Demo Airport scenario YAML."""

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
    nodes = {node["id"]: node for node in definition["airport"]["nodes"]}
    edges = definition["airport"]["edges"]

    def point(node_id: str) -> tuple[float, float]:
        node = nodes[node_id]
        return 105.0 + float(node["x_m"]), 425.0 - float(node["y_m"])

    parts = [
        '<svg xmlns="http://www.w3.org/2000/svg" width="1120" height="850" viewBox="0 0 1120 850">',
        '<rect width="1120" height="850" fill="#f4f6f8"/>',
        '<text x="50" y="42" font-family="Segoe UI, sans-serif" font-size="25" font-weight="700" fill="#182b3a">RampLab Demo Airport</text>',
        '<text x="50" y="68" font-family="Segoe UI, sans-serif" font-size="14" fill="#536675">Operational graph topology • local meters • schematic, not to scale</text>',
        '<rect x="900" y="95" width="190" height="104" rx="8" fill="#fff" stroke="#aab7c1"/>',
        '<text x="915" y="120" font-family="Segoe UI, sans-serif" font-size="14" font-weight="700" fill="#182b3a">Runway 09/27</text>',
        '<text x="915" y="143" font-family="Segoe UI, sans-serif" font-size="12" fill="#536675">1,400 × 30 m authored</text>',
        '<text x="915" y="163" font-family="Segoe UI, sans-serif" font-size="12" fill="#536675">timed resource in engine</text>',
        '<text x="915" y="183" font-family="Segoe UI, sans-serif" font-size="12" fill="#536675">pavement not in route graph</text>',
    ]

    for edge in edges:
        x1, y1 = point(edge["from"])
        x2, y2 = point(edge["to"])
        service_only = not edge.get("aircraft_allowed", True)
        color = "#2f855a" if service_only else "#597b96"
        dash = ' stroke-dasharray="6 4"' if service_only else ""
        width = "3.4" if service_only else "2.4"
        parts.append(
            f'<line x1="{x1:.1f}" y1="{y1:.1f}" x2="{x2:.1f}" y2="{y2:.1f}" '
            f'stroke="{color}" stroke-width="{width}"{dash} stroke-linecap="round" opacity="0.9"/>'
        )

    for node_id, node in nodes.items():
        x, y = point(node_id)
        if node_id.startswith(("gate_", "arrival_gate_")):
            fill, radius = "#805ad5", 8
        elif "stand" in node_id:
            fill, radius = "#dd6b20", 6
        elif "exit" in node_id:
            fill, radius = "#c53030", 7
        elif node_id == "depot":
            fill, radius = "#2f855a", 8
        else:
            fill, radius = "#263746", 6
        parts.append(f'<circle cx="{x:.1f}" cy="{y:.1f}" r="{radius}" fill="{fill}" stroke="#fff" stroke-width="2"/>')
        label = html.escape(node_id.replace("_", " "))
        parts.append(
            f'<text x="{x + 10:.1f}" y="{y - 9:.1f}" font-family="Segoe UI, sans-serif" '
            f'font-size="11" fill="#263746">{label}</text>'
        )

    parts.extend([
        '<path d="M 62 785 L 62 735 M 62 735 L 56 746 M 62 735 L 68 746" fill="none" stroke="#263746" stroke-width="2"/>',
        '<text x="52" y="720" font-family="Segoe UI, sans-serif" font-size="12" fill="#263746">N</text>',
        '<text x="78" y="780" font-family="Segoe UI, sans-serif" font-size="12" fill="#263746">+X east</text>',
        '<line x1="875" y1="760" x2="915" y2="760" stroke="#597b96" stroke-width="3"/><text x="925" y="765" font-family="Segoe UI, sans-serif" font-size="12" fill="#263746">aircraft / shared taxiway</text>',
        '<line x1="875" y1="785" x2="915" y2="785" stroke="#2f855a" stroke-width="3" stroke-dasharray="6 4"/><text x="925" y="790" font-family="Segoe UI, sans-serif" font-size="12" fill="#263746">GSE-only service link</text>',
        '<text x="50" y="825" font-family="Segoe UI, sans-serif" font-size="12" fill="#536675">Runway thresholds and pavement are authored metadata; this view shows only graph nodes and links consumed by routing.</text>',
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
