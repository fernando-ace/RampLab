"""Validate canonical simulator movement events and prepare Unreal replay rows."""
from __future__ import annotations

import json
import math
from pathlib import Path
from typing import Any


def unreal_replay_rows(events_path: Path) -> tuple[list[dict[str, Any]], dict[str, int]]:
    rows: list[dict[str, Any]] = []
    counters = {"events": 0, "with_pose": 0, "without_pose": 0}
    previous_time = -math.inf
    for line_number, line in enumerate(events_path.read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip():
            continue
        event = json.loads(line)
        counters["events"] += 1
        timestamp = event.get("time_seconds")
        if not isinstance(timestamp, (int, float)) or timestamp < previous_time:
            raise ValueError(f"event line {line_number} has missing or non-monotonic simulation time")
        previous_time = timestamp
        entities = (
            (event.get("entity_id"), event.get("entity_type"), event.get("position_m"),
             event.get("heading_rad"), event.get("state")),
            (event.get("related_entity_id"), event.get("related_entity_type"), event.get("related_position_m"),
             event.get("related_heading_rad"), event.get("related_state")),
        )
        for entity_id, entity_type, position, heading, state in entities:
            if not entity_id:
                continue
            if entity_type not in {"aircraft", "ground_vehicle"}:
                counters["without_pose"] += 1
                continue
            if (not isinstance(position, dict) or not isinstance(position.get("x"), (int, float))
                    or not isinstance(position.get("y"), (int, float)) or not isinstance(heading, (int, float))):
                counters["without_pose"] += 1
                continue
            east_m, north_m = float(position["x"]), float(position["y"])
            rows.append({
                "time_seconds": timestamp, "sequence": event.get("sequence"),
                "entity_id": entity_id, "entity_type": entity_type,
                "event_type": event.get("type"), "route_edge_id": event.get("edge_id"),
                "task_id": event.get("task_id"), "state": state,
                "source_position_m": {"east": east_m, "north": north_m},
                "unreal_location_cm": {"x": round(100 * east_m, 3), "y": round(-100 * north_m, 3), "z": 0.0},
                "source_heading_rad": float(heading),
                "unreal_yaw_degrees": round(-math.degrees(float(heading)), 6),
                "interpolation": "none; event sample only",
            })
            counters["with_pose"] += 1
    return rows, counters


def write_unreal_replay(events_path: Path, output_path: Path) -> dict[str, Any]:
    rows, counters = unreal_replay_rows(events_path)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w", encoding="utf-8", newline="\n") as stream:
        for row in rows:
            stream.write(json.dumps(row, sort_keys=True, separators=(",", ":")) + "\n")
    return {**counters, "output": str(output_path), "status": "PASS" if rows else "NO_POSITION_SAMPLES"}
