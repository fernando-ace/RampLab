#!/usr/bin/env python3
"""Package the Goal 37 policy comparison with the existing evidence bundle tool."""
from __future__ import annotations

import argparse
import csv
import json
import subprocess
import sys
import tempfile
from pathlib import Path


METRIC_KEYS = (
    "simulated_duration_seconds", "agent_decisions", "agent_route_selections",
    "agent_replans", "agent_objectives_completed", "agent_objectives_failed",
    "mean_departure_delay_seconds", "surface_departed_aircraft", "surface_total_aircraft",
    "runway_operations_completed", "surface_reroutes", "surface_wait_events",
    "surface_wait_seconds", "surface_taxi_distance_m", "surface_taxi_seconds",
    "runway_queue_seconds", "surface_safe_failures", "max_simultaneous_taxiing_aircraft",
    "surface_aircraft_aircraft_collisions", "surface_aircraft_ground_collisions",
    "minimum_aircraft_separation_m", "minimum_aircraft_ground_separation_m",
)


def make_input(evidence_root: Path, experiment: dict, policy: str, target: Path) -> None:
    target.mkdir(parents=True)
    rows = []
    event_lines = []
    for item in experiment["runs"]:
        if item["policy"] != policy or item["scenario"] != "coordinated_route_choice":
            continue
        run_dir = evidence_root / item["directory"]
        metrics = json.loads((run_dir / "metrics.json").read_text(encoding="utf-8"))
        row = {"scenario": item["scenario"], "seed": item["seed"]}
        row.update({key: metrics[key] for key in METRIC_KEYS if key in metrics})
        row["surface_departed_aircraft"] = metrics["surface"]["departed_aircraft"]
        row["surface_total_aircraft"] = metrics["surface"]["total_aircraft"]
        row["surface_reroutes"] = metrics["surface"]["reroutes"]
        row["surface_wait_events"] = metrics["surface"]["wait_events"]
        row["surface_wait_seconds"] = metrics["surface"]["wait_seconds"]
        row["surface_taxi_distance_m"] = metrics["surface"]["taxi_distance_m"]
        row["surface_taxi_seconds"] = metrics["surface"]["taxi_seconds"]
        row["runway_queue_seconds"] = metrics["surface"]["runway_queue_seconds"]
        row["surface_safe_failures"] = metrics["surface"]["safe_failures"]
        row["max_simultaneous_taxiing_aircraft"] = metrics["surface"]["max_simultaneous_taxiing"]
        row["surface_aircraft_aircraft_collisions"] = metrics["surface"]["aircraft_aircraft_collisions"]
        row["surface_aircraft_ground_collisions"] = metrics["surface"]["aircraft_ground_collisions"]
        row["minimum_aircraft_separation_m"] = metrics["surface"]["minimum_aircraft_separation_m"]
        row["minimum_aircraft_ground_separation_m"] = metrics["surface"]["minimum_aircraft_ground_separation_m"]
        rows.append(row)
        for line in (run_dir / "events.jsonl").read_text(encoding="utf-8").splitlines():
            if line.strip():
                event = json.loads(line)
                event.update({"scenario": item["scenario"], "seed": item["seed"], "policy": policy})
                event_lines.append(json.dumps(event, sort_keys=True, separators=(",", ":")))

    summary = {"schema_version": 1, "experiment_name": f"Goal 37 {policy} coordinated route-choice fixture",
        "source_scenario": "coordinated_route_choice.yaml", "run_count": len(rows),
        "seeds": {"values": experiment["seeds"]}, "runs": rows}
    (target / "experiment.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    with (target / "runs.csv").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    # The bundle validator expects aircraft detail for one representative
    # simulator run, while experiment-level KPIs aggregate all 30 runs.
    representative = next(item for item in experiment["runs"]
        if item["policy"] == policy and item["scenario"] == "coordinated_route_choice" and item["seed"] == 42)
    representative_metrics = json.loads((evidence_root / representative["directory"] / "metrics.json")
        .read_text(encoding="utf-8"))
    aircraft_rows = []
    for aircraft in representative_metrics["aircraft_operations"]:
        aircraft_rows.append({"scenario": representative["scenario"], "seed": representative["seed"],
            "policy": policy, **{key: json.dumps(value, sort_keys=True) if isinstance(value, (dict, list)) else value
                for key, value in aircraft.items()}})
    with (target / "aircraft.csv").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(aircraft_rows[0]))
        writer.writeheader()
        writer.writerows(aircraft_rows)
    (target / "events.jsonl").write_text("\n".join(event_lines) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--evidence-root", type=Path, default=Path("docs/goal37-evidence"))
    parser.add_argument("--output", type=Path, default=Path("docs/goal37-evidence/portable-bundle"))
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[2]
    evidence_root = (repo / args.evidence_root).resolve()
    output = (repo / args.output).resolve()
    experiment = json.loads((evidence_root / "experiment.json").read_text(encoding="utf-8"))
    with tempfile.TemporaryDirectory(prefix="goal37-bundle-", dir=repo / "build-goal37") as scratch:
        scratch_path = Path(scratch)
        adaptive = scratch_path / "adaptive"
        coordinated = scratch_path / "coordinated"
        make_input(evidence_root, experiment, "ADAPTIVE", adaptive)
        make_input(evidence_root, experiment, "COORDINATED_ADAPTIVE", coordinated)
        determinism = scratch_path / "determinism.json"
        checks = experiment["determinism"]["checks"]
        report = {"determinism": {str(index): {"status": "byte_identical" if check["passed"] else "fail"}
            for index, check in enumerate(checks)}}
        determinism.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        subprocess.run([sys.executable, "-m", "tools.evidence_bundle", "--control", str(adaptive),
            "--disruption", str(coordinated), "--output", str(output),
            "--determinism-report", str(determinism)], cwd=repo, check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
