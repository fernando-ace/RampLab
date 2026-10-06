#!/usr/bin/env python3
"""Validate a simulator-generated Goal 21 run bundle across its artifacts."""

from __future__ import annotations

import csv
import json
import math
import sys
from pathlib import Path
from typing import Any


REQUIRED = ("experiment.json", "runs.csv", "aircraft.csv", "events.jsonl")


def _number(value: Any, label: str) -> float:
    try:
        result = float(value)
    except (TypeError, ValueError) as exc:
        raise ValueError(f"{label} is not numeric") from exc
    if not math.isfinite(result):
        raise ValueError(f"{label} is not finite")
    return result


def validate_bundle(directory: str | Path) -> dict[str, Any]:
    root = Path(directory)
    missing = [name for name in REQUIRED if not (root / name).is_file()]
    if missing:
        raise ValueError(f"incomplete run bundle; missing: {', '.join(missing)}")

    metadata = json.loads((root / "experiment.json").read_text(encoding="utf-8"))
    if metadata.get("schema_version") != 1 or metadata.get("run_count") != 1:
        raise ValueError("experiment.json must contain one schema-version-1 run")
    runs = metadata.get("runs")
    if not isinstance(runs, list) or len(runs) != 1 or not isinstance(runs[0], dict):
        raise ValueError("experiment.json must include exactly one run record")
    run = runs[0]

    with (root / "runs.csv").open(newline="", encoding="utf-8-sig") as handle:
        rows = list(csv.DictReader(handle))
    if len(rows) != 1:
        raise ValueError("runs.csv must include exactly one run row")
    csv_run = rows[0]
    if str(metadata.get("seed")) != csv_run.get("seed") or str(run.get("seed")) != csv_run.get("seed"):
        raise ValueError("seed differs between experiment.json and runs.csv")
    for key, value in (("scenario", run.get("scenario")), ("scenario", metadata.get("scenario_name"))):
        if value != csv_run.get(key):
            raise ValueError("scenario identity differs between experiment.json and runs.csv")

    with (root / "aircraft.csv").open(newline="", encoding="utf-8-sig") as handle:
        aircraft = list(csv.DictReader(handle))
    if len(aircraft) != int(_number(run.get("aircraft_count"), "aircraft_count")):
        raise ValueError("aircraft row count differs from exported aircraft_count")
    aircraft_ids = [row.get("aircraft_id") for row in aircraft]
    flights = [row.get("flight_number") for row in aircraft]
    if len(set(aircraft_ids)) != len(aircraft_ids) or len(set(flights)) != len(flights):
        raise ValueError("aircraft.csv contains duplicate aircraft IDs or flight numbers")
    completed = sum(int(_number(row.get("completed_tasks"), "completed_tasks")) for row in aircraft)
    tasks = sum(int(_number(row.get("task_count"), "task_count")) for row in aircraft)
    departed = sum(row.get("state") == "Departed" for row in aircraft)
    delayed = sum(_number(row.get("departure_delay_seconds"), "departure_delay_seconds") > 0 for row in aircraft)
    for key, value in (("completed_service_tasks", completed), ("service_task_count", tasks),
                       ("delayed_aircraft", delayed), ("aircraft_count", len(aircraft))):
        if int(_number(run.get(key), key)) != value or int(_number(csv_run.get(key), key)) != value:
            raise ValueError(f"{key} differs across aircraft.csv, runs.csv, and experiment.json")
    if not aircraft:
        raise ValueError("aircraft.csv contains no aircraft rows")
    expected_averages = {
        "avg_turnaround_minutes": sum(_number(row.get("turnaround_duration_seconds"), "turnaround_duration_seconds")
                                       for row in aircraft) / len(aircraft) / 60.0,
        "avg_departure_delay_minutes": sum(_number(row.get("departure_delay_seconds"), "departure_delay_seconds")
                                            for row in aircraft) / len(aircraft) / 60.0,
        "avg_service_waiting_minutes": sum(_number(row.get("service_waiting_seconds"), "service_waiting_seconds")
                                            for row in aircraft) / len(aircraft) / 60.0,
    }
    for key, expected in expected_averages.items():
        if not math.isclose(_number(run.get(key), key), expected, rel_tol=0.0, abs_tol=1e-5) or \
                not math.isclose(_number(csv_run.get(key), key), expected, rel_tol=0.0, abs_tol=1e-5):
            raise ValueError(f"{key} differs from the aircraft-level results")

    events = []
    for line_number, line in enumerate((root / "events.jsonl").read_text(encoding="utf-8").splitlines(), start=1):
        if not line.strip():
            continue
        try:
            event = json.loads(line)
        except json.JSONDecodeError as exc:
            raise ValueError(f"events.jsonl line {line_number} is invalid JSON: {exc.msg}") from exc
        if not isinstance(event, dict):
            raise ValueError(f"events.jsonl line {line_number} must be an object")
        events.append(event)
    for event in events:
        if "route_nodes" in event or "route_node_names" in event:
            ids, names = event.get("route_nodes"), event.get("route_node_names")
            if not isinstance(ids, list) or not isinstance(names, list) or len(ids) != len(names):
                raise ValueError("recorded route node IDs and names must be aligned arrays")
    sequences = [_number(event.get("sequence"), "event sequence") for event in events]
    if any(right <= left for left, right in zip(sequences, sequences[1:])):
        raise ValueError("event sequence numbers must increase strictly")
    if any(_number(right.get("time_seconds"), "event time") < _number(left.get("time_seconds"), "event time")
           for left, right in zip(events, events[1:])):
        raise ValueError("event timestamps are not in deterministic order")

    event_departures = sum(event.get("type") == "AircraftDeparted" for event in events)
    event_services = sum(event.get("type") == "ServiceCompleted" for event in events)
    event_disruptions = sum(event.get("type") in {"RoadClosed", "RoadOpened"} for event in events)
    event_closures = sum(event.get("type") == "RoadClosed" for event in events)
    if event_departures != departed:
        raise ValueError("AircraftDeparted event count differs from aircraft.csv")
    if event_services != completed:
        raise ValueError("ServiceCompleted event count differs from aircraft.csv")
    if event_disruptions != int(_number(run.get("disruption_events"), "disruption_events")):
        raise ValueError("road disruption event count differs from the run metrics")
    if int(_number(run.get("disruption_events"), "disruption_events")) != int(_number(csv_run.get("disruption_events"), "disruption_events")):
        raise ValueError("disruption event count differs between runs.csv and experiment.json")
    if event_closures != int(_number(run.get("road_closure_events"), "road_closure_events")) or \
            event_closures != int(_number(csv_run.get("road_closure_events"), "road_closure_events")):
        raise ValueError("RoadClosed event count differs from the run metrics")
    if len(events) != int(_number(run.get("event_count"), "event_count")) or \
            len(events) != int(_number(csv_run.get("event_count"), "event_count")):
        raise ValueError("ordered event count differs between events.jsonl and run metrics")
    if not events or not math.isclose(_number(events[-1].get("time_seconds"), "event time"),
                                      _number(run.get("simulated_duration_seconds"), "simulated duration"),
                                      rel_tol=0.0, abs_tol=1e-9):
        raise ValueError("final event time differs from simulated_duration_seconds")
    if int(_number(run.get("aircraft_count"), "aircraft_count")) != len(aircraft):
        raise ValueError("metadata aircraft total differs from aircraft.csv")

    return {"directory": str(root.resolve()), "scenario": run["scenario"], "seed": int(run["seed"]),
            "aircraft": len(aircraft), "departed": departed, "service_tasks": tasks,
            "completed_service_tasks": completed, "disruption_events": event_disruptions,
            "road_closure_events": event_closures, "event_count": len(events),
            "collision_data": "unknown (not exported by this simulator)"}


def main(argv: list[str] | None = None) -> int:
    args = sys.argv[1:] if argv is None else argv
    if len(args) != 1:
        print("Usage: python validate_real_bundle.py RUN_DIRECTORY", file=sys.stderr)
        return 2
    try:
        print(json.dumps(validate_bundle(args[0]), indent=2))
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"Invalid run bundle: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
