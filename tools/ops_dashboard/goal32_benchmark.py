"""Run and compare a real serial and parallel Goal 32 KAUO sweep."""
from __future__ import annotations

import argparse
from copy import deepcopy
import json
from pathlib import Path
import time
from datetime import datetime, timezone

from tools.ops_dashboard import scenario_studio
from tools.ops_dashboard.experiments import ExperimentManager


def definition(workers: int, scenario: dict, name: str | None = None) -> dict:
    return {
        "name": name or f"KAUO Goal32 {'serial' if workers == 1 else 'parallel'} parity run",
        "description": "Real native simulator sweep over mapped route closure timing and departure schedules with three deterministic seeds.",
        "base_dataset": scenario["dataset_id"], "base_scenario": scenario,
        "parameters": [
            {"id": "disruption.route_closure.start_time", "values": [f"2026-10-09T14:{minute:02d}:00Z" for minute in (0, 15, 30, 45)]},
            {"id": "disruption.route_closure.duration_minutes", "values": [15, 30, 45]},
            {"id": "flight.KAUO-ILLUSTRATIVE-DEP.scheduled_time", "values": ["2026-10-09T15:00:00Z", "2026-10-09T15:15:00Z"]},
        ],
        "seeds": [42, 43, 44], "include_baseline": True,
        "execution": {"workers": workers, "cache_enabled": False},
        "constraints": [
            {"metric": key, "op": "eq", "value": 0} for key in (
                "surface_aircraft_aircraft_collisions", "surface_aircraft_ground_collisions", "surface_safe_failures")
        ] + [{"metric": "surface_departed_aircraft", "op": "eq", "value": 1}],
        "ranking": {"objectives": [{"metric": "avg_departure_delay_minutes", "direction": "minimize"}]},
        "metrics_of_interest": ["avg_departure_delay_minutes", "surface_taxi_seconds", "surface_aircraft_aircraft_collisions",
                                "surface_aircraft_ground_collisions", "surface_safe_failures", "surface_departed_aircraft"],
    }


def execute(manager: ExperimentManager, definition_body: dict) -> tuple[dict, float]:
    workers = definition_body["execution"]["workers"]
    experiment = manager.create(definition_body)
    print(json.dumps({"phase": "created", "id": experiment["id"], "workers": workers,
                      "configurations": experiment["counts"]["configurations"], "runs": experiment["counts"]["total"]}), flush=True)
    started = time.perf_counter()
    manager.start(experiment["id"], workers=workers)
    thread = manager._futures[experiment["id"]]
    last = -1
    while thread.is_alive():
        thread.join(0.5)
        current = manager.get(experiment["id"])
        completed = current["counts"]["completed"]
        if completed != last:
            print(json.dumps({"phase": "progress", "id": experiment["id"], "completed": completed,
                              "running": current["counts"]["running"], "queued": current["counts"]["queued"]}), flush=True)
            last = completed
    elapsed = time.perf_counter() - started
    return manager.get(experiment["id"]), elapsed


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=scenario_studio.ROOT / "docs" / "goal32-demo-benchmark.json")
    parser.add_argument("--workers", type=int, choices=(1, 4), help="Run one real sweep instead of the serial/parallel pair.")
    args = parser.parse_args()
    manager = ExperimentManager(scenario_studio.HOME, scenario_studio)
    scenario = scenario_studio.new_scenario("kauo")
    if args.workers:
        name = "KAUO Goal32 progress capture " + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        result, elapsed = execute(manager, definition(args.workers, scenario, name))
        print(json.dumps({"phase": "complete", "id": result["id"], "status": result["status"],
                          "wall_seconds": round(elapsed, 3), "counts": result["counts"]}, indent=2), flush=True)
        return 0 if result["status"] == "COMPLETED" else 1
    serial_definition = definition(1, scenario)
    parallel_definition = definition(4, scenario)
    serial, serial_seconds = execute(manager, serial_definition)
    parallel, parallel_seconds = execute(manager, parallel_definition)
    serial_runs = {row["run_id"]: row for row in serial["runs"]}
    parallel_runs = {row["run_id"]: row for row in parallel["runs"]}
    parity = len(serial_runs) == len(parallel_runs) and all(
        serial_runs[key].get("run_identity") == parallel_runs[key].get("run_identity")
        and serial_runs[key].get("result_digest") == parallel_runs[key].get("result_digest")
        and serial_runs[key]["status"] == parallel_runs[key]["status"] == "COMPLETED"
        for key in serial_runs.keys() & parallel_runs.keys())
    winner = next((row for row in parallel["ranking_result"] if row.get("configuration_id") == parallel.get("selected_candidate")), None)
    baseline = next((row for row in parallel["ranking_result"] if row["configuration_id"].startswith("baseline_")), None)
    candidate_delay = winner.get("metrics", {}).get("avg_departure_delay_minutes", {}).get("mean") if winner else None
    baseline_delay = baseline.get("metrics", {}).get("avg_departure_delay_minutes", {}).get("mean") if baseline else None
    summary = {"schema_version": 1, "dataset": parallel["base_dataset"], "configurations": parallel["counts"]["configurations"],
               "seeds": parallel["seeds"], "runs_per_sweep": parallel["counts"]["total"],
               "serial": {"experiment_id": serial["id"], "workers": 1, "wall_seconds": round(serial_seconds, 3), "counts": serial["counts"]},
               "parallel": {"experiment_id": parallel["id"], "workers": 4, "wall_seconds": round(parallel_seconds, 3), "counts": parallel["counts"]},
               "speedup": round(serial_seconds / parallel_seconds, 3) if parallel_seconds else None,
               "serial_parallel_parity": parity,
               "successful_result_digests_equal": sum(serial_runs[key].get("result_digest") == parallel_runs[key].get("result_digest")
                                                       for key in serial_runs.keys() & parallel_runs.keys()),
               "best_tested_candidate": {"configuration_id": winner["configuration_id"], "status": winner["status"],
                                         "parameters": winner["parameters"], "mean_departure_delay_minutes": candidate_delay} if winner else None,
               "baseline_mean_departure_delay_minutes": baseline_delay,
               "candidate_minus_baseline_delay_minutes": round(candidate_delay - baseline_delay, 3)
               if candidate_delay is not None and baseline_delay is not None else None,
               "run_identities": {key: serial_runs[key].get("run_identity") for key in sorted(serial_runs)},
               "result_digests": {key: parallel_runs[key].get("result_digest") for key in sorted(parallel_runs)},
               "software_fingerprint": parallel_runs[sorted(parallel_runs)[0]].get("software_fingerprint") if parallel_runs else None,
               "provenance": parallel["provenance"]}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(summary, ensure_ascii=False, sort_keys=True, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"phase": "complete", "summary": str(args.output), "serial_parallel_parity": parity,
                      "serial_seconds": summary["serial"]["wall_seconds"], "parallel_seconds": summary["parallel"]["wall_seconds"],
                      "speedup": summary["speedup"], "selected_candidate": parallel.get("selected_candidate")}, indent=2), flush=True)
    return 0 if parity and serial["status"] == parallel["status"] == "COMPLETED" else 1


if __name__ == "__main__":
    raise SystemExit(main())
