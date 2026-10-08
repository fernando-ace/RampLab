"""Adapt native simulator exports to the existing Goal 19 and Goal 22 contracts."""
from __future__ import annotations

import csv
import json
import math
from pathlib import Path
from typing import Any


_NATIVE_METRICS = {
    "fleet": {
        "collisions": "fleet_collisions",
        "minimum_separation_m": "fleet_minimum_separation_m",
        "reservation_contentions": "fleet_reservation_contentions",
        "outstanding_reservations": "fleet_outstanding_reservations",
        "unfinished_requests": "fleet_unfinished_requests",
        "reassignments": "fleet_reassignments",
        "requests_created": "fleet_requests_created",
        "requests_completed": "fleet_requests_completed",
        "requests_failed": "fleet_requests_failed",
    },
    "surface": {
        "departed_aircraft": "surface_departed_aircraft",
        "total_aircraft": "surface_total_aircraft",
        "departure_throughput_per_hour": "surface_departure_throughput_per_hour",
        "reroutes": "surface_reroutes",
        "wait_events": "surface_wait_events",
        "wait_seconds": "surface_wait_seconds",
        "taxi_distance_m": "surface_taxi_distance_m",
        "taxi_seconds": "surface_taxi_seconds",
        "runway_queue_seconds": "runway_queue_seconds",
        "safe_failures": "surface_safe_failures",
        "max_simultaneous_taxiing": "max_simultaneous_taxiing_aircraft",
        "minimum_aircraft_separation_m": "minimum_aircraft_separation_m",
        "minimum_aircraft_ground_separation_m": "minimum_aircraft_ground_separation_m",
        "aircraft_aircraft_collisions": "surface_aircraft_aircraft_collisions",
        "aircraft_ground_collisions": "surface_aircraft_ground_collisions",
        "arrivals_completed": "surface_arrived_aircraft",
        "gate_assignments": "gate_assignments",
        "gate_wait_seconds": "gate_wait_seconds",
        "gate_occupancy_seconds": "gate_occupancy_seconds",
        "arrival_to_departure_seconds": "arrival_to_departure_seconds",
        "runway_operations_completed": "runway_operations_completed",
        "maximum_runway_queue_depth": "maximum_runway_queue_depth",
        "arrival_runway_wait_seconds": "arrival_runway_wait_seconds",
        "departure_runway_wait_seconds": "departure_runway_wait_seconds",
        "average_runway_wait_seconds": "average_runway_wait_seconds",
        "runway_utilization": "runway_utilization",
        "arrival_taxi_seconds": "arrival_taxi_seconds",
        "arrival_taxi_distance_m": "arrival_taxi_distance_m",
        "departure_taxi_seconds": "departure_taxi_seconds",
        "departure_taxi_distance_m": "departure_taxi_distance_m",
    },
}

_TOP_LEVEL_METRICS = (
    "simulated_duration_seconds",
    "total_turnarounds",
    "completed_turnarounds",
    "delayed_turnarounds",
    "failed_or_timed_out_turnarounds",
    "task_reassignments",
    "disruption_triggered_replans",
    "unresolved_service_requests",
)


def flatten_native_metrics(native: dict[str, Any], events: list[dict[str, Any]] | None = None) -> dict[str, Any]:
    """Translate the C++ CLI's nested metrics into Goal 19 metric names."""
    row = {key: native[key] for key in _TOP_LEVEL_METRICS if key in native}
    for section, mapping in _NATIVE_METRICS.items():
        values = native.get(section, {})
        if not isinstance(values, dict):
            continue
        row.update({target: values[source] for source, target in mapping.items() if source in values})

    turns = native.get("turnarounds", [])
    aircraft = native.get("aircraft_operations", [])
    turns = [turn for turn in turns if isinstance(turn, dict)] if isinstance(turns, list) else []
    aircraft = [item for item in aircraft if isinstance(item, dict)] if isinstance(aircraft, list) else []
    delays = [float(turn["departure_delay_seconds"]) for turn in turns
              if isinstance(turn.get("departure_delay_seconds"), (int, float))]
    durations = [float(turn["estimated_ready_time_seconds"]) - float(turn["actual_arrival_seconds"])
                 for turn in turns
                 if isinstance(turn.get("estimated_ready_time_seconds"), (int, float))
                 and isinstance(turn.get("actual_arrival_seconds"), (int, float))]
    waits = [max(0.0, float(task["started_at_seconds"]) - float(task["requested_at_seconds"]))
             for turn in turns for task in turn.get("tasks", [])
             if isinstance(task, dict)
             and isinstance(task.get("started_at_seconds"), (int, float))
             and isinstance(task.get("requested_at_seconds"), (int, float))]
    surface = native.get("surface", {})
    aircraft_count = surface.get("total_aircraft") if isinstance(surface, dict) else None
    row.update({
        "aircraft_count": aircraft_count if isinstance(aircraft_count, (int, float)) else len(aircraft),
        "avg_departure_delay_minutes": math.fsum(delays) / len(delays) / 60.0 if delays else 0.0,
        "avg_turnaround_minutes": math.fsum(durations) / len(durations) / 60.0 if durations else 0.0,
        "avg_service_waiting_minutes": math.fsum(waits) / len(waits) / 60.0 if waits else 0.0,
        "delayed_aircraft": int(native.get("delayed_turnarounds", 0)),
        "total_service_task_wait_seconds": math.fsum(waits),
        "maximum_service_task_wait_seconds": max(waits, default=0.0),
        "road_closure_events": sum(
            event.get("type") in {"RoadClosed", "RoadAvailabilityChanged"} for event in (events or [])
        ),
    })
    return row


def _aircraft_rows(native: dict[str, Any]) -> list[dict[str, Any]]:
    turns = native.get("turnarounds", [])
    operations = native.get("aircraft_operations", [])
    if not isinstance(turns, list) or not isinstance(operations, list):
        raise ValueError("native aircraft and turnaround metrics must be arrays")
    by_aircraft = {
        turn["aircraft_id"]: turn for turn in turns
        if isinstance(turn, dict) and "aircraft_id" in turn
    }
    rows = []
    for item in operations:
        if not isinstance(item, dict) or "aircraft_id" not in item:
            raise ValueError("native simulator aircraft_operations must contain aircraft_id objects")
        turn = by_aircraft.get(item["aircraft_id"], {})
        tasks = turn.get("tasks", []) if isinstance(turn, dict) else []
        tasks = tasks if isinstance(tasks, list) else []
        arrival_start = item.get("arrival_taxi_started_at_seconds")
        arrival_end = item.get("arrival_taxi_completed_at_seconds")
        departure_start = item.get("departure_taxi_started_at_seconds")
        departure_end = item.get("departure_taxi_completed_at_seconds")
        taxi_seconds = sum(
            end - start for start, end in ((arrival_start, arrival_end), (departure_start, departure_end))
            if isinstance(start, (int, float)) and isinstance(end, (int, float))
        )
        arrival = turn.get("actual_arrival_seconds") if isinstance(turn, dict) else None
        ready = turn.get("estimated_ready_time_seconds") if isinstance(turn, dict) else None
        rows.append({
            "aircraft_id": item["aircraft_id"],
            "flight_number": item.get("flight_number", ""),
            "turnaround_id": turn.get("turnaround_id", "") if isinstance(turn, dict) else "",
            "operation_type": item.get("operation_type", ""),
            "surface_state": item.get("surface_state", "Unknown"),
            "state": item.get("surface_state", "Unknown"),
            "taxi_time_seconds": taxi_seconds,
            "taxi_distance_m": item.get("taxi_distance_m", 0),
            "total_operational_delay_seconds": turn.get("departure_delay_seconds", 0) if isinstance(turn, dict) else 0,
            "departure_delay_seconds": turn.get("departure_delay_seconds", "") if isinstance(turn, dict) else "",
            "turnaround_duration_seconds": ready - arrival if isinstance(ready, (int, float)) and isinstance(arrival, (int, float)) else "",
            "runway_wait_seconds": item.get("runway_wait_seconds", 0),
            "gate_arrival_time_seconds": item.get("arrival_gate_time_seconds", ""),
            "departure_time_seconds": item.get("departure_time_seconds", ""),
            "task_count": len(tasks),
            "completed_tasks": sum(task.get("state") == "Completed" for task in tasks if isinstance(task, dict)),
            "service_waiting_seconds": math.fsum(
                max(0.0, task["started_at_seconds"] - task["requested_at_seconds"])
                for task in tasks if isinstance(task, dict)
                and isinstance(task.get("started_at_seconds"), (int, float))
                and isinstance(task.get("requested_at_seconds"), (int, float))
            ),
        })
    return rows


def _compatibility_metadata(scenario_file: Path) -> dict[str, Any] | None:
    parent = scenario_file.resolve().parent
    names = ("manifest.json", "identity-map.json", "support-matrix.json")
    if not all((parent / name).is_file() for name in names):
        return None
    return {
        "manifest": json.loads((parent / names[0]).read_text(encoding="utf-8")),
        "identity_map": json.loads((parent / names[1]).read_text(encoding="utf-8")),
        "support_matrix": json.loads((parent / names[2]).read_text(encoding="utf-8")),
        "scenario_document": json.loads(scenario_file.resolve().read_text(encoding="utf-8")),
    }


def write_goal19_artifacts(
    output: Path,
    native_metrics_path: Path,
    scenario_name: str,
    scenario_file: Path,
) -> list[str]:
    """Write compatibility artifacts while retaining the simulator's originals."""
    native = json.loads(native_metrics_path.read_text(encoding="utf-8"))
    if not isinstance(native, dict):
        raise ValueError("native simulator metrics must be a JSON object")
    seed = native.get("seed")
    if not isinstance(seed, int) or isinstance(seed, bool):
        raise ValueError("native simulator metrics must contain an integer seed")
    row = {"scenario": scenario_name, "seed": seed, "ordinal": 1}
    generation = _compatibility_metadata(scenario_file)
    event_path = output / "events.jsonl"
    events = [
        json.loads(line)
        for line in event_path.read_text(encoding="utf-8").splitlines()
        if line.strip()
    ] if event_path.is_file() else []
    if any(not isinstance(event, dict) for event in events):
        raise ValueError("native events.jsonl must contain only JSON objects")
    row.update(flatten_native_metrics(native, events))
    experiment: dict[str, Any] = {
        "experiment_name": scenario_name,
        "source_scenario": scenario_name,
        "source_scenario_file": scenario_file.name,
        "schema_version": "1.0",
        "run_count": 1,
        "seeds": {"values": [seed]},
        "runs": [row],
        "native_metrics": native,
        "events": events,
    }
    if generation is not None:
        experiment["scenario_generation"] = generation
    experiment_path = output / "experiment.json"
    experiment_path.write_text(json.dumps(experiment, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    csv_path = output / "runs.csv"
    columns = list(row)
    with csv_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=columns, extrasaction="ignore", lineterminator="\n")
        writer.writeheader()
        writer.writerow(row)

    aircraft = _aircraft_rows(native)
    aircraft_columns = list(aircraft[0]) if aircraft else []
    with (output / "aircraft.csv").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=aircraft_columns, extrasaction="ignore", lineterminator="\n")
        if aircraft_columns:
            writer.writeheader()
            writer.writerows(aircraft)

    return [experiment_path.name, csv_path.name, "aircraft.csv"]


def make_determinism_report(
    control: Path,
    control_repeat: Path,
    disruption: Path,
    disruption_repeat: Path,
) -> dict[str, Any]:
    """Compare the two scenario pairs in the exact four-check Goal 22 contract."""
    from tools.experiment_analysis.analyze import determinism, load_run

    def compare_scenario_pair(label: str, first: Path, second: Path) -> dict[str, Any]:
        left = load_run(first / "experiment.json")
        right = load_run(second / "experiment.json")
        for key in ("source_scenario", "seed"):
            if left.identity.get(key) != right.identity.get(key):
                raise ValueError(f"{label} repeat runs have different {key} values")
        left_generation = left.raw.get("scenario_generation")
        right_generation = right.raw.get("scenario_generation")
        if not isinstance(left_generation, dict) or not isinstance(right_generation, dict):
            raise ValueError(f"{label} repeat runs are missing generated-scenario provenance")
        if left_generation != right_generation:
            raise ValueError(f"{label} repeat runs have different generated-scenario provenance")
        return determinism(left, right)

    for directory in (control, control_repeat, disruption, disruption_repeat):
        event_file = directory / "events.jsonl"
        if not event_file.is_file() or not any(
            line.strip() for line in event_file.read_text(encoding="utf-8").splitlines()
        ):
            raise ValueError(f"repeat-run evidence requires a nonempty event log: {event_file}")

    comparisons = {
        "control": compare_scenario_pair("control", control, control_repeat),
        "disruption": compare_scenario_pair("disruption", disruption, disruption_repeat),
        "control_events": determinism(
            load_run(control / "events.jsonl"), load_run(control_repeat / "events.jsonl")
        ),
        "disruption_events": determinism(
            load_run(disruption / "events.jsonl"), load_run(disruption_repeat / "events.jsonl")
        ),
    }
    return {
        "schema_version": "1.0",
        "determinism": comparisons,
    }


def write_determinism_report(
    control: Path,
    control_repeat: Path,
    disruption: Path,
    disruption_repeat: Path,
    output: Path,
) -> dict[str, Any]:
    report = make_determinism_report(control, control_repeat, disruption, disruption_repeat)
    output = output.expanduser().resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return report
