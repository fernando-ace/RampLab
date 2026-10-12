#!/usr/bin/env python3
"""Run the fixed Goal 35 policy matrix with shared seeds and emit inspectable evidence."""
from __future__ import annotations

import argparse
import concurrent.futures
import csv
import hashlib
import json
import re
import shutil
import subprocess
from pathlib import Path

MATRIX = {
    "normal_operations": "turnaround_lifecycle.yaml",
    "congestion": "surface_traffic.yaml",
    "temporary_route_disruption": "surface_traffic_disrupted.yaml",
    "competing_aircraft_demand": "mixed_runway_operations.yaml",
    "combined_stress": "mixed_runway_disrupted.yaml",
    "adaptive_route_choice_demo": "goal35_route_choice_demo.yaml",
}
POLICIES = ("SCRIPTED", "ADAPTIVE")


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def configure_scenario(source: Path, target: Path, policy: str) -> None:
    text = source.read_text(encoding="utf-8")
    pattern = re.compile(r"^(surface_operations:\s*\{)(.*)(\})$", re.MULTILINE)
    match = pattern.search(text)
    if not match:
        raise RuntimeError(f"surface_operations inline map missing from {source}")
    fields = re.sub(r",?\s*aircraft_routing_policy:\s*(?:SCRIPTED|ADAPTIVE)", "", match.group(2))
    replacement = f"{match.group(1)}{fields.rstrip()}, aircraft_routing_policy: {policy}{match.group(3)}"
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(text[: match.start()] + replacement + text[match.end() :], encoding="utf-8")


def run_one(cli: Path, label: str, scenario: Path, output: Path, seed: int, policy: str) -> dict:
    output.mkdir(parents=True, exist_ok=True)
    metrics = output / "metrics.json"
    events = output / "events.jsonl"
    decisions = output / "decisions.json"
    command = [str(cli), "--quiet", "--scenario", str(scenario), "--seed", str(seed),
               "--metrics-json", str(metrics), "--record-events", str(events),
               "--agent-decisions", str(decisions)]
    completed = subprocess.run(command, cwd=cli.parent.parent, capture_output=True, text=True)
    if completed.returncode:
        raise RuntimeError(f"run failed ({completed.returncode}): {' '.join(command)}\n{completed.stderr}\n{completed.stdout}")
    metric_data = json.loads(metrics.read_text(encoding="utf-8"))
    decision_data = json.loads(decisions.read_text(encoding="utf-8"))
    return {"scenario": label, "seed": seed, "policy": policy,
            "directory": f"runs/{label}/seed-{seed}/{policy.lower()}",
            "metrics": metric_data, "decision_count": len(decision_data["decisions"]),
            "digests": {"metrics.json": digest(metrics), "events.jsonl": digest(events),
                        "decisions.json": digest(decisions)}}


def normalized(metrics: dict) -> dict:
    surface = metrics["surface"]
    aircraft = metrics["aircraft_operations"]
    return {"seed": metrics["seed"], "simulated_duration_seconds": metrics["simulated_duration_seconds"],
            "agent_decisions": metrics["agent_decisions"], "agent_replans": metrics["agent_replans"],
            "agent_objectives_completed": metrics["agent_objectives_completed"],
            "agent_objectives_failed": metrics["agent_objectives_failed"],
            "surface": {key: value for key, value in surface.items()},
            "aircraft_operations": aircraft}


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--output", type=Path, default=None)
    parser.add_argument("--seeds", type=int, nargs="+", default=[42, 43, 44])
    parser.add_argument("--workers", type=int, default=4)
    args = parser.parse_args()
    repo = args.repo.resolve()
    output = (args.output or repo / "docs" / "goal35-evidence").resolve()
    configs = output / "config" / "scenarios"
    output.mkdir(parents=True, exist_ok=True)
    configured: dict[tuple[str, str], Path] = {}
    for label, filename in MATRIX.items():
        source = repo / "scenarios" / filename
        for policy in POLICIES:
            target = configs / f"{label}-{policy.lower()}.yaml"
            configure_scenario(source, target, policy)
            configured[(label, policy)] = target

    configuration = {"schema_version": 1, "policies": list(POLICIES), "scenarios": {
        label: {"source": f"scenarios/{filename}", "source_sha256": digest(repo / "scenarios" / filename),
                "configured_scenarios": {policy: {"path": f"config/scenarios/{label}-{policy.lower()}.yaml",
                    "sha256": digest(configured[(label, policy)])} for policy in POLICIES}}
        for label, filename in MATRIX.items()}, "seeds": args.seeds,
        "serial_runs": len(MATRIX) * len(args.seeds) * len(POLICIES), "parallel_replay_workers": args.workers,
        "configuration_delta": "surface_operations.aircraft_routing_policy only"}
    (output / "config" / "experiment_config.json").write_text(json.dumps(configuration, indent=2) + "\n", encoding="utf-8")

    entries = [(label, seed, policy) for label in MATRIX for seed in args.seeds for policy in POLICIES]
    serial_rows = []
    for label, seed, policy in entries:
        serial_rows.append(run_one(args.cli.resolve(), label, configured[(label, policy)],
            output / "runs" / label / f"seed-{seed}" / policy.lower(), seed, policy))

    scratch = output / ".parallel-check"
    if scratch.exists():
        shutil.rmtree(scratch)
    parallel_rows: dict[tuple[str, int, str], dict] = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as executor:
        futures = {}
        for label, seed, policy in entries:
            future = executor.submit(run_one, args.cli.resolve(), label, configured[(label, policy)],
                scratch / label / f"seed-{seed}" / policy.lower(), seed, policy)
            futures[future] = (label, seed, policy)
        for future in concurrent.futures.as_completed(futures):
            parallel_rows[futures[future]] = future.result()

    checks = []
    for row in serial_rows:
        key = (row["scenario"], row["seed"], row["policy"])
        parallel = parallel_rows[key]
        serial_metrics = normalized(row["metrics"])
        parallel_metrics = normalized(parallel["metrics"])
        same = serial_metrics == parallel_metrics and row["digests"] == parallel["digests"]
        checks.append({"scenario": key[0], "seed": key[1], "policy": key[2], "normalized_metrics_match": serial_metrics == parallel_metrics,
                       "artifact_sha256_match": row["digests"] == parallel["digests"], "passed": same})
    shutil.rmtree(scratch)
    if not all(item["passed"] for item in checks):
        raise RuntimeError("serial and parallel normalized evidence differs")

    table_rows = []
    for label in MATRIX:
        for policy in POLICIES:
            runs = [row for row in serial_rows if row["scenario"] == label and row["policy"] == policy]
            metric_list = [row["metrics"] for row in runs]
            surfaces = [item["surface"] for item in metric_list]
            delays = [m["mean_departure_delay_seconds"] for m in metric_list]
            table_rows.append({"scenario": label, "policy": policy, "runs": len(runs),
                "mean_departure_delay_seconds": sum(delays) / len(delays),
                "mean_departure_taxi_seconds": sum(s["departure_taxi_seconds"] for s in surfaces) / len(surfaces),
                "mean_departure_taxi_distance_m": sum(s["departure_taxi_distance_m"] for s in surfaces) / len(surfaces),
                "mean_surface_wait_seconds": sum(s["wait_seconds"] for s in surfaces) / len(surfaces),
                "mean_runway_queue_seconds": sum(s["runway_queue_seconds"] for s in surfaces) / len(surfaces),
                "completed_objectives": sum(m["agent_objectives_completed"] for m in metric_list),
                "failed_objectives": sum(m["agent_objectives_failed"] for m in metric_list),
                "replans": sum(m["agent_replans"] for m in metric_list),
                "decisions": sum(m["agent_decisions"] for m in metric_list),
                "collisions": sum(s["aircraft_aircraft_collisions"] + s["aircraft_ground_collisions"] for s in surfaces),
                "safety_failures": sum(s["safe_failures"] for s in surfaces),
                "minimum_separation_m": min(s["minimum_aircraft_separation_m"] for s in surfaces)})

    with (output / "results.csv").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(table_rows[0]))
        writer.writeheader()
        writer.writerows(table_rows)
    manifest = {"schema_version": 1, "policy_matrix": list(POLICIES), "scenarios": MATRIX,
        "seeds": args.seeds, "serial_runs": len(serial_rows), "parallel_replay_runs": len(checks),
        "workers": args.workers, "determinism": {"checks": checks, "passed": all(item["passed"] for item in checks)},
        "results": table_rows, "runs": [{k: v for k, v in row.items() if k != "metrics"} for row in serial_rows]}
    (output / "experiment.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"serial_runs": len(serial_rows), "parallel_replays": len(checks),
                      "determinism_passed": manifest["determinism"]["passed"], "results": table_rows}, indent=2))


if __name__ == "__main__":
    main()
