#!/usr/bin/env python3
"""Validate a simulator-generated Goal 21 run bundle across its artifacts."""

from __future__ import annotations

import csv
import json
import math
import sys
from pathlib import Path
from typing import Any


REQUIRED = ("experiment.json", "runs.csv", "aircraft.csv", "events.jsonl",
            "simulator-metrics.json", "simulator-metrics.csv", "simulator-metrics.aircraft.csv")


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
    source_metrics = json.loads((root / "simulator-metrics.json").read_text(encoding="utf-8"))
    surface = source_metrics.get("surface", {})
    with (root / "simulator-metrics.csv").open(newline="", encoding="utf-8-sig") as handle:
        native_run_rows = list(csv.DictReader(handle))
    if not native_run_rows:
        raise ValueError("simulator-metrics.csv contains no rows")
    if len(aircraft) != int(_number(run.get("aircraft_count"), "aircraft_count")):
        raise ValueError("aircraft row count differs from exported aircraft_count")
    aircraft_ids = [row.get("aircraft_id") for row in aircraft]
    flights = [row.get("flight_number") for row in aircraft]
    if len(set(aircraft_ids)) != len(aircraft_ids) or len(set(flights)) != len(flights):
        raise ValueError("aircraft.csv contains duplicate aircraft IDs or flight numbers")
    completed = sum(int(_number(row.get("completed_tasks"), "completed_tasks")) for row in aircraft)
    tasks = sum(int(_number(row.get("task_count"), "task_count")) for row in aircraft)
    departed = sum(row.get("state") == "Departed" for row in aircraft)
    for key, value in (("completed_service_tasks", completed), ("service_task_count", tasks),
                       ("aircraft_count", len(aircraft)), ("surface_departed_aircraft", departed),
                       ("surface_total_aircraft", len(aircraft))):
        if int(_number(run.get(key), key)) != value or int(_number(csv_run.get(key), key)) != value:
            raise ValueError(f"{key} differs across aircraft.csv, runs.csv, and experiment.json")
    if not aircraft:
        raise ValueError("aircraft.csv contains no aircraft rows")
    turns = source_metrics.get("turnarounds", [])
    if len(turns) != int(_number(run.get("total_turnarounds"), "total_turnarounds")):
        raise ValueError("turnaround totals differ between normalized and native simulator metrics")
    native_completed = sum(int(_number(turn.get("completed_required_tasks"), "completed_required_tasks"))
                           for turn in turns)
    native_tasks = sum(len(turn.get("tasks", [])) for turn in turns)
    if native_completed != completed or native_tasks != tasks:
        raise ValueError("service task totals differ from simulator-metrics.json")
    departures = [row for row in aircraft if row.get("operation_type") == "departure"]
    delay_values = [_number(row.get("departure_delay_seconds"), "departure_delay_seconds")
                    for row in departures if row.get("departure_delay_seconds") not in (None, "")]
    turnaround_values = [_number(row.get("turnaround_duration_seconds"), "turnaround_duration_seconds")
                         for row in departures if row.get("turnaround_duration_seconds") not in (None, "")]
    delayed_aircraft = sum(value > 0 for value in delay_values)
    if delayed_aircraft != int(_number(run.get("delayed_aircraft"), "delayed_aircraft")):
        raise ValueError("delayed aircraft count differs between aircraft.csv and run metrics")
    for key, values in (("avg_departure_delay_minutes", delay_values), ("avg_turnaround_minutes", turnaround_values)):
        expected = sum(values) / len(values) / 60.0 if values else 0.0
        if not math.isclose(_number(run.get(key), key), expected, rel_tol=0.0, abs_tol=1e-5):
            raise ValueError(f"{key} differs from aircraft-level simulator values")
    native_aircraft = {str(item["aircraft_id"]): item for item in source_metrics.get("aircraft_operations", [])}
    if set(aircraft_ids) != set(native_aircraft):
        raise ValueError("aircraft.csv identities differ from simulator-metrics.json")
    expected_taxi_distance = _number(surface.get("taxi_distance_m"), "taxi_distance_m")
    actual_taxi_distance = sum(_number(row.get("taxi_distance_m"), "taxi_distance_m") for row in aircraft)
    if not math.isclose(actual_taxi_distance, expected_taxi_distance, rel_tol=0.0, abs_tol=1e-5):
        raise ValueError("taxi distance differs between aircraft.csv and simulator-metrics.json")
    native_fields = {
        "seed": "seed", "simulated_duration_seconds": "simulated_duration_seconds",
        "surface_departed_aircraft": "surface_departed_aircraft",
        "surface_total_aircraft": "surface_total_aircraft",
        "surface_aircraft_aircraft_collisions": "surface_aircraft_aircraft_collisions",
        "surface_aircraft_ground_collisions": "surface_aircraft_ground_collisions",
        "minimum_aircraft_separation_m": "minimum_aircraft_separation_m",
    }
    for output_key, native_key in native_fields.items():
        expected = _number(run.get(output_key), output_key)
        for native_row in native_run_rows:
            tolerance = 1e-3 if "separation_m" in output_key else 1e-5
            if not math.isclose(_number(native_row.get(native_key), native_key), expected,
                                rel_tol=0.0, abs_tol=tolerance):
                raise ValueError(f"{output_key} differs between normalized and native metrics CSV")
    for native_row in native_run_rows:
        native_taxi_distance = sum(_number(native_row.get(key), key) for key in
                                   ("arrival_taxi_distance_m", "departure_taxi_distance_m"))
        if not math.isclose(native_taxi_distance, expected_taxi_distance, rel_tol=0.0, abs_tol=1e-5):
            raise ValueError("surface taxi distance differs between normalized and native metrics CSV")

    with (root / "simulator-metrics.aircraft.csv").open(newline="", encoding="utf-8-sig") as handle:
        native_aircraft_rows = list(csv.DictReader(handle))
    native_aircraft = {str(item["aircraft_id"]): item for item in native_aircraft_rows}
    if set(aircraft_ids) != set(native_aircraft):
        raise ValueError("aircraft identities differ between normalized and native aircraft CSV")
    for row in aircraft:
        native = native_aircraft[str(row["aircraft_id"])]
        for key in ("flight_number", "operation_type", "surface_state"):
            if row.get(key) != native.get(key):
                raise ValueError(f"aircraft {key} differs between normalized and native aircraft CSV")
        for key in ("taxi_distance_m", "runway_wait_seconds", "departure_time_seconds"):
            if row.get(key) not in (None, "") and native.get(key) not in (None, "") and not math.isclose(
                    _number(row[key], key), _number(native[key], key), rel_tol=0.0, abs_tol=1e-5):
                raise ValueError(f"aircraft {key} differs between normalized and native aircraft CSV")

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
        if "route_nodes" in event and not isinstance(event["route_nodes"], list):
            raise ValueError("recorded route node IDs must be an array")
        if "route_nodes" in event and "route_node_names" in event:
            ids, names = event["route_nodes"], event["route_node_names"]
            if not isinstance(names, list) or len(ids) != len(names):
                raise ValueError("recorded route node IDs and names must be aligned arrays")
    sequences = [_number(event.get("sequence"), "event sequence") for event in events]
    if any(right <= left for left, right in zip(sequences, sequences[1:])):
        raise ValueError("event sequence numbers must increase strictly")
    if any(_number(right.get("time_seconds"), "event time") < _number(left.get("time_seconds"), "event time")
           for left, right in zip(events, events[1:])):
        raise ValueError("event timestamps are not in deterministic order")

    event_departures = sum(event.get("type") == "AircraftDeparted" for event in events)
    event_services = sum(event.get("type") == "TurnaroundTaskCompleted" for event in events)
    event_disruptions = sum(event.get("type") in {"RoadClosed", "RoadOpened"} for event in events)
    event_closures = sum(event.get("type") == "RoadClosed" for event in events)
    if event_departures != departed:
        raise ValueError("AircraftDeparted event count differs from aircraft.csv")
    if event_services != completed:
        raise ValueError("TurnaroundTaskCompleted event count differs from aircraft.csv")
    if event_disruptions != int(_number(run.get("disruption_events"), "disruption_events")):
        raise ValueError("road disruption event count differs from the event history")
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
    surface_collisions = sum(int(_number(surface.get(key), key)) for key in
                             ("aircraft_aircraft_collisions", "aircraft_ground_collisions"))
    exported_collisions = int(_number(run.get("surface_aircraft_aircraft_collisions"), "aircraft collisions")) + \
        int(_number(run.get("surface_aircraft_ground_collisions"), "aircraft-ground collisions"))
    if surface_collisions != exported_collisions:
        raise ValueError("collision totals differ between normalized and native simulator metrics")
    if not math.isclose(_number(run.get("minimum_aircraft_separation_m"), "minimum separation"),
                        _number(surface.get("minimum_aircraft_separation_m"), "minimum separation"),
                        rel_tol=0.0, abs_tol=1e-6):
        raise ValueError("minimum separation differs between normalized and native simulator metrics")

    return {"directory": str(root.resolve()), "scenario": run["scenario"], "seed": int(run["seed"]),
            "aircraft": len(aircraft), "departed": departed, "service_tasks": tasks,
            "completed_service_tasks": completed, "disruption_events": event_disruptions,
            "road_closure_events": event_closures, "event_count": len(events),
            "aircraft_collisions": surface_collisions,
            "minimum_aircraft_separation_m": surface.get("minimum_aircraft_separation_m")}


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
