#!/usr/bin/env python3
"""Generate and analyze the canonical simulator-backed Goal 21 experiment."""

from __future__ import annotations

import argparse
import csv
import importlib.util
import json
import math
import subprocess
import sys
from pathlib import Path
from typing import Any

from validate_real_bundle import validate_bundle


ROOT = Path(__file__).resolve().parents[2]
ANALYZER_PATH = ROOT / "tools" / "experiment_analysis" / "analyze.py"
SPEC = importlib.util.spec_from_file_location("ramplab_experiment_analysis", ANALYZER_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"Goal 19 analyzer not found at {ANALYZER_PATH}")
ANALYZER = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = ANALYZER
SPEC.loader.exec_module(ANALYZER)


def _read_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def _normalized_metrics(source: dict[str, Any], events: list[dict[str, Any]]) -> dict[str, Any]:
    row: dict[str, Any] = {key: value for key, value in source.items()
                           if key in ANALYZER.METRICS and isinstance(value, (int, float))}
    for key, value in source.get("fleet", {}).items():
        if isinstance(value, (int, float)):
            metric = f"fleet_{key}"
            if metric in ANALYZER.METRICS:
                row[metric] = value
    aliases = {
        "departed_aircraft": "surface_departed_aircraft",
        "total_aircraft": "surface_total_aircraft",
        "reroutes": "surface_reroutes",
        "wait_events": "surface_wait_events",
        "wait_seconds": "surface_wait_seconds",
        "taxi_distance_m": "surface_taxi_distance_m",
        "taxi_seconds": "surface_taxi_seconds",
        "safe_failures": "surface_safe_failures",
        "max_simultaneous_taxiing": "max_simultaneous_taxiing_aircraft",
        "aircraft_aircraft_collisions": "surface_aircraft_aircraft_collisions",
        "aircraft_ground_collisions": "surface_aircraft_ground_collisions",
        "arrivals_completed": "surface_arrived_aircraft",
    }
    for key, value in source.get("surface", {}).items():
        metric = aliases.get(key, key)
        if metric in ANALYZER.METRICS and isinstance(value, (int, float)):
            row[metric] = value

    turns = source.get("turnarounds", [])
    ops = source.get("aircraft_operations", [])
    completed_tasks = sum(int(turn.get("completed_required_tasks", 0)) for turn in turns)
    task_count = sum(len(turn.get("tasks", [])) for turn in turns)
    delays = [float(turn["departure_delay_seconds"]) for turn in turns
              if isinstance(turn.get("departure_delay_seconds"), (int, float))]
    durations = [float(turn["estimated_ready_time_seconds"]) - float(turn.get("actual_arrival_seconds", 0))
                 for turn in turns if isinstance(turn.get("estimated_ready_time_seconds"), (int, float))]
    row.update({
        "aircraft_count": int(source.get("surface", {}).get("total_aircraft", len(ops))),
        "avg_departure_delay_minutes": math.fsum(delays) / len(delays) / 60.0 if delays else 0.0,
        "avg_turnaround_minutes": math.fsum(durations) / len(durations) / 60.0 if durations else 0.0,
        "delayed_aircraft": int(source.get("delayed_turnarounds", 0)),
        "completed_service_tasks": completed_tasks,
        "service_task_count": task_count,
        "event_count": len(events),
        "road_closure_events": sum(event.get("type") in {"RoadClosed", "RoadAvailabilityChanged"}
                                   for event in events),
        "disruption_events": sum("Disruption" in str(event.get("type", "")) or
                                  event.get("type") in {"RoadClosed", "RoadAvailabilityChanged"}
                                  for event in events),
    })
    return row


def _package_run(cli: Path, scenario: str, output: Path, name: str, seed: int) -> None:
    output.mkdir(parents=True, exist_ok=True)
    metrics_path = output / "simulator-metrics.json"
    metrics_csv = output / "simulator-metrics.csv"
    events_path = output / "events.jsonl"
    command = [str(cli), "--scenario", f"scenarios/{scenario}.yaml", "--seed", str(seed), "--quiet",
               "--metrics-json", str(metrics_path), "--metrics-csv", str(metrics_csv),
               "--record-events", str(events_path)]
    subprocess.run(command, cwd=ROOT, check=True, capture_output=True, text=True)
    source = _read_json(metrics_path)
    events = [json.loads(line) for line in events_path.read_text(encoding="utf-8").splitlines() if line.strip()]
    row = {"ordinal": 1, "case_id": "case_0001", "seed": seed, "replication": 1,
           "scenario": scenario, **_normalized_metrics(source, events)}
    metadata = {"schema_version": 1, "experiment_name": "goal21_mixed_runway_acceptance",
                "scenario_name": scenario, "source_scenario": f"scenarios/{scenario}.yaml",
                "seed": seed, "run_count": 1, "runs": [row], "outputs": [
                    "runs.csv", "aircraft.csv", "events.jsonl", "simulator-metrics.json",
                    "simulator-metrics.csv", "simulator-metrics.aircraft.csv"]}
    (output / "experiment.json").write_text(json.dumps(metadata, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    fieldnames = list(row)
    with (output / "runs.csv").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerow(row)

    turns = {turn["aircraft_id"]: turn for turn in source.get("turnarounds", [])}
    aircraft_rows = []
    for item in source.get("aircraft_operations", []):
        turn = turns.get(item["aircraft_id"], {})
        tasks = turn.get("tasks", [])
        arrival_start = item.get("arrival_taxi_started_at_seconds")
        arrival_end = item.get("arrival_taxi_completed_at_seconds")
        departure_start = item.get("departure_taxi_started_at_seconds")
        departure_end = item.get("departure_taxi_completed_at_seconds")
        taxi_seconds = sum(end - start for start, end in ((arrival_start, arrival_end),
                          (departure_start, departure_end)) if isinstance(start, (int, float))
                          and isinstance(end, (int, float)))
        aircraft_rows.append({
            "aircraft_id": item["aircraft_id"], "flight_number": item.get("flight_number", ""),
            "turnaround_id": turn.get("turnaround_id", ""), "operation_type": item.get("operation_type", ""),
            "surface_state": item.get("surface_state", "Unknown"), "state": item.get("surface_state", "Unknown"),
            "taxi_time_seconds": taxi_seconds, "taxi_distance_m": item.get("taxi_distance_m", 0),
            "total_operational_delay_seconds": turn.get("departure_delay_seconds", 0),
            "departure_delay_seconds": turn.get("departure_delay_seconds", ""),
            "turnaround_duration_seconds": (turn.get("estimated_ready_time_seconds", "") -
                turn.get("actual_arrival_seconds", 0)) if turn else "",
            "runway_wait_seconds": item.get("runway_wait_seconds", 0),
            "gate_arrival_time_seconds": item.get("arrival_gate_time_seconds", ""),
            "departure_time_seconds": item.get("departure_time_seconds", ""),
            "task_count": len(tasks),
            "completed_tasks": sum(task.get("state") == "Completed" for task in tasks),
            "service_waiting_seconds": sum(max(0, task.get("started_at_seconds", 0) -
                task.get("requested_at_seconds", 0)) for task in tasks),
        })
    if not aircraft_rows:
        raise ValueError(f"simulator did not export aircraft records for {scenario}")
    with (output / "aircraft.csv").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(aircraft_rows[0]))
        writer.writeheader()
        writer.writerows(aircraft_rows)
    # Keep the native per-aircraft operations output as a separately named source artifact.
    native_aircraft = metrics_csv.with_name(metrics_csv.stem + ".aircraft.csv")
    (output / "simulator-metrics.aircraft.csv").write_bytes(native_aircraft.read_bytes())


def generate(cli: Path, output: Path, seed: int) -> dict[str, Any]:
    cli = cli.resolve()
    output.mkdir(parents=True, exist_ok=True)
    scenarios = {"control": "mixed_runway_operations", "disruption": "mixed_runway_disrupted",
                 "control-repeat": "mixed_runway_operations", "disruption-repeat": "mixed_runway_disrupted"}
    summaries = {}
    for name, scenario in scenarios.items():
        destination = output / name
        _package_run(cli, scenario, destination, name, seed)
        summaries[name] = validate_bundle(destination)

    control = ANALYZER.load_run(output / "control" / "experiment.json")
    disruption = ANALYZER.load_run(output / "disruption" / "experiment.json")
    comparison = ANALYZER.compare(control, disruption)
    control_repeat = ANALYZER.load_run(output / "control-repeat" / "experiment.json")
    disruption_repeat = ANALYZER.load_run(output / "disruption-repeat" / "experiment.json")
    control_determinism = ANALYZER.determinism(control, control_repeat)
    disruption_determinism = ANALYZER.determinism(disruption, disruption_repeat)
    control_events = ANALYZER.load_run(output / "control" / "events.jsonl")
    control_repeat_events = ANALYZER.load_run(output / "control-repeat" / "events.jsonl")
    disruption_events = ANALYZER.load_run(output / "disruption" / "events.jsonl")
    disruption_repeat_events = ANALYZER.load_run(output / "disruption-repeat" / "events.jsonl")
    control_event_determinism = ANALYZER.determinism(control_events, control_repeat_events)
    disruption_event_determinism = ANALYZER.determinism(disruption_events, disruption_repeat_events)
    route_changes = []
    control_reroutes = [event for event in control_events.events if "Rerout" in str(event.get("type", ""))]
    disruption_reroutes = [event for event in disruption_events.events if "Rerout" in str(event.get("type", ""))]

    report = ANALYZER.markdown(comparison, control, disruption)
    report += "\n## Taxi route and disruption observations\n\n"
    report += (f"- Recorded reroute events: control {len(control_reroutes)}, disruption {len(disruption_reroutes)}.\n"
               f"- Total taxi distance: {control.metrics.get('surface_taxi_distance_m', 0):.1f} m control, "
               f"{disruption.metrics.get('surface_taxi_distance_m', 0):.1f} m disruption.\n")
    report += "\n## Repeated-run determinism\n\n"
    for label, result in (("Control metrics", control_determinism), ("Disruption metrics", disruption_determinism),
                          ("Control ordered events", control_event_determinism),
                          ("Disruption ordered events", disruption_event_determinism)):
        report += f"- {label}: **{result['status']}** — {result['message']}\n"
    result = {"schema_version": 1, "comparison": comparison,
              "determinism": {"control": control_determinism, "disruption": disruption_determinism,
                              "control_events": control_event_determinism,
                              "disruption_events": disruption_event_determinism},
              "event_comparison": {"control_reroute_events": len(control_reroutes),
                                   "disruption_reroute_events": len(disruption_reroutes)},
              "bundle_validation": summaries}
    (output / "analysis.md").write_text(report, encoding="utf-8")
    (output / "analysis.json").write_text(json.dumps(result, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    if comparison["safety"]["status"] == "regression":
        raise RuntimeError("the selected disruption regressed safety metrics; inspect analysis.json")
    if any(item["status"] not in {"equivalent", "byte_identical"} for item in
           (control_determinism, disruption_determinism, control_event_determinism, disruption_event_determinism)):
        raise RuntimeError("repeated seeded runs were not deterministic; see analysis.json")
    return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", type=Path, help="Path to the built airside_cli executable.")
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build", help="CMake build directory.")
    parser.add_argument("--output", type=Path, default=ROOT / "results" / "goal21-real")
    parser.add_argument("--seed", type=int, default=42)
    args = parser.parse_args(argv)
    cli = args.cli
    if cli is None:
        candidates = (args.build_dir / "Release" / "airside_cli.exe", args.build_dir / "airside_cli.exe",
                      args.build_dir / "airside_cli")
        cli = next((path for path in candidates if path.is_file()), candidates[0])
    try:
        result = generate(cli, args.output.resolve(), args.seed)
        print(json.dumps({"output": str(args.output.resolve()),
                          "kpis": result["comparison"]["metrics"],
                          "safety": result["comparison"]["safety"],
                          "determinism": {key: value["status"] for key, value in result["determinism"].items()}},
                         indent=2, allow_nan=False))
    except (OSError, ValueError, subprocess.CalledProcessError, RuntimeError) as exc:
        print(f"Goal 21 real experiment failed: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
