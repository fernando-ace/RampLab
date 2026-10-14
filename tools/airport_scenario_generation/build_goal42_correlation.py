"""Build measured simulator/Dashboard-event/Unreal-adapter correlation samples."""
from __future__ import annotations

import json
import math
from pathlib import Path
from typing import Any


def correlate(events_path: Path, unreal_rows_path: Path) -> dict[str, Any]:
    events = [json.loads(line) for line in events_path.read_text(encoding="utf-8").splitlines() if line.strip()]
    unreal_rows = [json.loads(line) for line in unreal_rows_path.read_text(encoding="utf-8").splitlines() if line.strip()]
    indexed = {(row.get("sequence"), row.get("entity_id")): row for row in unreal_rows}
    selections = [
        ("arrival", lambda e: e.get("type") == "AircraftLanded" and e.get("aircraft") == "KAUO-G31-GA-01", "entity_id"),
        ("departure", lambda e: e.get("type") == "SurfaceRunwayClearance" and e.get("aircraft") == "KAUO-G31-GA-03" and e.get("time_seconds", 0) > 1000, "entity_id"),
        ("pushback_disruption", lambda e: e.get("type") == "TurnaroundDisruptionDetected" and e.get("aircraft") == "KAUO-G31-GA-03", "entity_id"),
        ("ground_service", lambda e: e.get("type") == "TurnaroundTaskStarted" and e.get("vehicle") == "G31-FUEL-01" and e.get("related_entity_id"), "related_entity_id"),
    ]
    samples = []
    for label, predicate, id_field in selections:
        candidates = [event for event in events if predicate(event)]
        if not candidates:
            raise ValueError(f"no canonical event matched required correlation sample {label}")
        event = candidates[-1] if label == "departure" else candidates[0]
        entity_id = event[id_field]
        adapter = indexed.get((event.get("sequence"), entity_id))
        if adapter is None:
            raise ValueError(f"no Unreal adapter row for {label} entity {entity_id}")
        position = event.get("related_position_m") if id_field == "related_entity_id" else event.get("position_m")
        heading = event.get("related_heading_rad") if id_field == "related_entity_id" else event.get("heading_rad")
        if not isinstance(position, dict) or not isinstance(heading, (int, float)):
            raise ValueError(f"canonical event pose is unavailable for {label}")
        unreal = adapter["unreal_location_cm"]
        reconstructed = {"east": unreal["x"] / 100.0, "north": -unreal["y"] / 100.0}
        error = math.hypot(reconstructed["east"] - float(position["x"]), reconstructed["north"] - float(position["y"]))
        samples.append({
            "sample": label,
            "sequence": event.get("sequence"),
            "time_seconds": event.get("time_seconds"),
            "event_type": event.get("type"),
            "entity_id": entity_id,
            "entity_name": event.get("vehicle") if id_field == "related_entity_id" else event.get("aircraft"),
            "state": event.get("related_state") if id_field == "related_entity_id" else event.get("state"),
            "route_edge_id": event.get("edge_id"),
            "task_id": event.get("task_id"),
            "simulator_position_enu_m": {"east": float(position["x"]), "north": float(position["y"])},
            "dashboard_event_position_enu_m": {"east": float(position["x"]), "north": float(position["y"])},
            "unreal_location_cm": unreal,
            "unreal_roundtrip_position_enu_m": reconstructed,
            "unreal_roundtrip_error_m": round(error, 9),
            "tolerance_m": 0.00002,
            "within_tolerance": error <= 0.00002,
            "interpolation": adapter["interpolation"],
        })
    if not all(row["within_tolerance"] for row in samples):
        raise ValueError("cross-surface transform error exceeds the configured tolerance")
    return {"schema_version": 1, "source_event_file": events_path.name,
            "source_event_count": len(events), "adapter_row_count": len(unreal_rows),
            "dashboard_position_source": "same canonical event object consumed by event replay; no independent dashboard localization",
            "unreal_tolerance_m": 0.00002, "samples": samples}


def write_correlation(events_path: Path, unreal_rows_path: Path, output_path: Path) -> dict[str, Any]:
    report = correlate(events_path, unreal_rows_path)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return report
