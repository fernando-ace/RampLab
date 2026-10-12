#!/usr/bin/env python3
"""Run the deterministic Goal 37 surface-agent policy matrix and preserve evidence."""
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
    "shared_bottleneck": ("surface_traffic.yaml", "Competing aircraft use a constrained taxiway and node."),
    "route_choice_interaction": ("goal35_route_choice_demo.yaml", "An agent can choose a direct or shared route."),
    "closure_disruption": ("surface_traffic_disrupted.yaml", "A deterministic route closure affects active traffic."),
    "priority_conflict": ("mixed_runway_operations.yaml", "Arrivals and departures compete for shared surface access."),
    "fairness_contention": ("surface_traffic.yaml", "Repeated bottleneck contention is used to inspect wait aging and completion order."),
    "non_beneficial_coordination": ("turnaround_lifecycle.yaml", "Low-conflict baseline where coordination may add cost without benefit."),
    "shared_runway_entry_queue": ("goal37_shared_runway_entry_queue.yaml", "Synthetic extended runway-entry occupancy creates a longer shared-edge queue."),
    "long_shared_corridor": ("goal37_long_shared_corridor.yaml", "Synthetic longer merge corridor makes route overlap more costly."),
    "early_closure_merge": ("goal37_early_closure_merge.yaml", "Synthetic early edge closure forces active routes to converge."),
    "coordinated_route_choice": ("goal37_coordinated_route_choice.yaml", "Synthetic route-choice case where shared-route cost changes one movement decision."),
}
POLICIES = ("SCRIPTED", "ADAPTIVE", "COORDINATED_ADAPTIVE")


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def configure_scenario(source: Path, target: Path, policy: str) -> None:
    text = source.read_text(encoding="utf-8")
    pattern = re.compile(r"^(surface_operations:\s*\{)(.*)(\})$", re.MULTILINE)
    match = pattern.search(text)
    if not match:
        raise RuntimeError(f"surface_operations inline map missing from {source}")
    fields = re.sub(r",?\s*aircraft_routing_policy:\s*(?:SCRIPTED|ADAPTIVE|COORDINATED_ADAPTIVE)", "", match.group(2))
    replacement = f"{match.group(1)}{fields.rstrip()}, aircraft_routing_policy: {policy}{match.group(3)}"
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(text[: match.start()] + replacement + text[match.end() :], encoding="utf-8")


def run_one(cli: Path, label: str, scenario: Path, output: Path, seed: int, policy: str) -> dict:
    output.mkdir(parents=True, exist_ok=True)
    metrics, events, decisions = (output / name for name in ("metrics.json", "events.jsonl", "decisions.json"))
    command = [str(cli), "--quiet", "--scenario", str(scenario), "--seed", str(seed),
               "--metrics-json", str(metrics), "--record-events", str(events), "--agent-decisions", str(decisions)]
    completed = subprocess.run(command, cwd=cli.parent.parent, capture_output=True, text=True)
    if completed.returncode:
        raise RuntimeError(f"run failed ({completed.returncode}): {' '.join(command)}\n{completed.stderr}\n{completed.stdout}")
    metric_data = json.loads(metrics.read_text(encoding="utf-8"))
    decision_data = json.loads(decisions.read_text(encoding="utf-8"))
    completion = {}
    for event in decision_data["decisions"]:
        if event["result"] == "OBJECTIVE_COMPLETED":
            completion.setdefault(event["agent_id"], []).append(event["time_seconds"])
    coordination = [event for event in decision_data["decisions"] if event.get("coordination", {}).get("rule")]
    return {"scenario": label, "seed": seed, "policy": policy,
            "directory": f"runs/{label}/seed-{seed}/{policy.lower()}", "metrics": metric_data,
            "decision_count": len(decision_data["decisions"]), "coordination_decisions": len(coordination),
            "yield_or_wait_decisions": sum(event["action"] == "WAIT" for event in coordination),
            "contention_decisions": sum(bool(event["observation"].get("shared_traffic")) or
                any(candidate.get("affected_agents", 0) > 0 for candidate in event["candidates"])
                for event in coordination),
            "selected_shared_conflict_decisions": sum(event["coordination"].get("affected_agents", 0) > 0
                for event in coordination),
            "agent_completion_times_seconds": completion,
            "digests": {name: digest(output / name) for name in ("metrics.json", "events.jsonl", "decisions.json")}}


def normalized(metrics: dict) -> dict:
    return {"seed": metrics["seed"], "simulated_duration_seconds": metrics["simulated_duration_seconds"],
            "agent_decisions": metrics["agent_decisions"], "agent_replans": metrics["agent_replans"],
            "agent_objectives_completed": metrics["agent_objectives_completed"],
            "agent_objectives_failed": metrics["agent_objectives_failed"],
            "surface": metrics["surface"], "aircraft_operations": metrics["aircraft_operations"]}


def summarize(label: str, policy: str, runs: list[dict]) -> dict:
    metrics = [row["metrics"] for row in runs]
    surfaces = [item["surface"] for item in metrics]
    mean = lambda values: (materialized := list(values)) and sum(materialized) / len(materialized)
    return {"scenario": label, "policy": policy, "runs": len(runs),
            "completion_successes": sum(item["agent_objectives_completed"] for item in metrics),
            "objective_failures": sum(item["agent_objectives_failed"] for item in metrics),
            "mean_surface_wait_seconds": mean(item["wait_seconds"] for item in surfaces),
            "mean_taxi_seconds": mean(item["taxi_seconds"] for item in surfaces),
            "mean_taxi_distance_m": mean(item["taxi_distance_m"] for item in surfaces),
            "mean_departure_delay_seconds": mean(item["mean_departure_delay_seconds"] for item in metrics),
            "mean_departure_throughput_per_hour": mean(item["departure_throughput_per_hour"] for item in surfaces),
            "collisions": sum(item["aircraft_aircraft_collisions"] + item["aircraft_ground_collisions"] for item in surfaces),
            "safety_failures": sum(item["safe_failures"] for item in surfaces),
            "minimum_aircraft_separation_m": min(item["minimum_aircraft_separation_m"] for item in surfaces),
            "reroutes": sum(item["reroutes"] for item in surfaces),
            "coordination_decisions": sum(row["coordination_decisions"] for row in runs),
            "yield_or_wait_decisions": sum(row["yield_or_wait_decisions"] for row in runs),
            "contention_decisions": sum(row["contention_decisions"] for row in runs),
            "selected_shared_conflict_decisions": sum(row["selected_shared_conflict_decisions"] for row in runs),
            "per_agent_completion_times_seconds": [row["agent_completion_times_seconds"] for row in runs],
            "mean_objective_completion_seconds": mean(float(t) for row in runs for times in row["agent_completion_times_seconds"].values() for t in times),
            "worst_objective_completion_seconds": max((float(t) for row in runs for times in row["agent_completion_times_seconds"].values() for t in times), default=0.0)}


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--output", type=Path, default=None)
    parser.add_argument("--seeds", type=int, nargs="+", default=[42, 43, 44])
    parser.add_argument("--workers", type=int, default=4)
    args = parser.parse_args()
    repo = args.repo.resolve()
    output = (args.output or repo / "docs" / "goal37-evidence").resolve()
    configs = output / "config" / "scenarios"
    output.mkdir(parents=True, exist_ok=True)
    configured = {}
    for label, (filename, _) in MATRIX.items():
        source = repo / "scenarios" / filename
        for policy in POLICIES:
            target = configs / f"{label}-{policy.lower()}.yaml"
            configure_scenario(source, target, policy)
            configured[(label, policy)] = target

    entries = [(label, seed, policy) for label in MATRIX for seed in args.seeds for policy in POLICIES]
    serial_rows = [run_one(args.cli.resolve(), label, configured[(label, policy)],
        output / "runs" / label / f"seed-{seed}" / policy.lower(), seed, policy)
        for label, seed, policy in entries]
    scratch = output / ".parallel-check"
    if scratch.exists():
        shutil.rmtree(scratch)
    parallel_rows = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as executor:
        futures = {executor.submit(run_one, args.cli.resolve(), label, configured[(label, policy)],
            scratch / label / f"seed-{seed}" / policy.lower(), seed, policy): (label, seed, policy)
            for label, seed, policy in entries}
        for future in concurrent.futures.as_completed(futures):
            parallel_rows[futures[future]] = future.result()

    checks = []
    for row in serial_rows:
        key = (row["scenario"], row["seed"], row["policy"])
        parallel = parallel_rows[key]
        checks.append({"scenario": key[0], "seed": key[1], "policy": key[2],
            "normalized_metrics_match": normalized(row["metrics"]) == normalized(parallel["metrics"]),
            "artifact_sha256_match": row["digests"] == parallel["digests"],
            "passed": normalized(row["metrics"]) == normalized(parallel["metrics"]) and row["digests"] == parallel["digests"]})
    shutil.rmtree(scratch)
    if not all(item["passed"] for item in checks):
        raise RuntimeError("serial and parallel normalized evidence differs")

    table = [summarize(label, policy, [row for row in serial_rows if row["scenario"] == label and row["policy"] == policy])
             for label in MATRIX for policy in POLICIES]
    with (output / "results.csv").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=[key for key, value in table[0].items() if key != "per_agent_completion_times_seconds"])
        writer.writeheader()
        writer.writerows([{key: value for key, value in row.items() if key != "per_agent_completion_times_seconds"} for row in table])
    experiment = {"schema_version": 1, "policy_matrix": list(POLICIES), "scenarios": {
        label: {"source": f"scenarios/{filename}", "purpose": purpose,
            "source_sha256": digest(repo / "scenarios" / filename),
            "configured_scenarios": {policy: {"path": f"config/scenarios/{label}-{policy.lower()}.yaml",
                "sha256": digest(configured[(label, policy)])} for policy in POLICIES}}
        for label, (filename, purpose) in MATRIX.items()}, "seeds": args.seeds,
        "serial_runs": len(serial_rows), "parallel_replay_runs": len(checks), "workers": args.workers,
        "configuration_delta": "surface_operations.aircraft_routing_policy only",
        "determinism": {"checks": checks, "passed": all(item["passed"] for item in checks)},
        "results": table, "runs": [{key: value for key, value in row.items() if key != "metrics"} for row in serial_rows]}
    (output / "config" / "experiment_config.json").write_text(json.dumps({
        "schema_version": 1, "policies": list(POLICIES), "seeds": args.seeds,
        "serial_runs": len(serial_rows), "parallel_replay_workers": args.workers,
        "configuration_delta": "surface_operations.aircraft_routing_policy only",
        "scenarios": experiment["scenarios"]}, indent=2) + "\n", encoding="utf-8")
    (output / "experiment.json").write_text(json.dumps(experiment, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"serial_runs": len(serial_rows), "parallel_replays": len(checks),
        "determinism_passed": experiment["determinism"]["passed"], "results": table}, indent=2))


if __name__ == "__main__":
    main()
