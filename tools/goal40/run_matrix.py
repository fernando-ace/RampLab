#!/usr/bin/env python3
"""Run Goal 40's deterministic six-policy counterfactual cost comparison."""
from __future__ import annotations

import argparse
import concurrent.futures
import csv
import hashlib
import json
import re
import shutil
import subprocess
import time
from pathlib import Path

MATRIX = {
    "shared_bottleneck": ("surface_traffic.yaml", "Moderate repeated contention for a shared taxiway and node."),
    "individual_route_choice": ("goal35_route_choice_demo.yaml", "Goal 35 individual local route-choice case."),
    "shared_route_choice": ("goal37_coordinated_route_choice.yaml", "Goal 37 shared-edge route choice and coordination case."),
    "staggered_runway_entry": ("goal37_shared_runway_entry_queue.yaml", "Synthetic runway-entry bottleneck with staggered aircraft arrivals."),
    "long_corridor_congestion": ("goal37_long_shared_corridor.yaml", "Synthetic congestion case with alternate corridor routes."),
    "opposing_intersecting_traffic": ("mixed_runway_operations.yaml", "Mixed arrival/departure traffic intersects shared surface access."),
    "closure_during_execution": ("surface_traffic_disrupted.yaml", "A deterministic route closure invalidates active routes during execution."),
    "little_coordination_benefit": ("turnaround_lifecycle.yaml", "Low-conflict baseline where coordination should add little benefit."),
    "early_closure_merge": ("goal37_early_closure_merge.yaml", "Synthetic early closure forces traffic to converge at a merge."),
    "difficult_local_choice": ("mixed_runway_disrupted.yaml", "A deliberately difficult intersecting-traffic case with a disruption during operations."),
}
POLICIES = ("SCRIPTED", "ADAPTIVE", "COORDINATED_ADAPTIVE", "ROLLING_HORIZON_COORDINATED",
            "COUNTERFACTUAL", "COST_AWARE_COUNTERFACTUAL")


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def configure_scenario(source: Path, target: Path, policy: str) -> None:
    text = source.read_text(encoding="utf-8")
    pattern = re.compile(r"^(surface_operations:\s*\{)(.*)(\})$", re.MULTILINE)
    match = pattern.search(text)
    if not match:
        raise RuntimeError(f"surface_operations inline map missing from {source}")
    fields = re.sub(r",?\s*aircraft_routing_policy:\s*(?:SCRIPTED|ADAPTIVE|COORDINATED_ADAPTIVE|ROLLING_HORIZON_COORDINATED|COUNTERFACTUAL|COST_AWARE_COUNTERFACTUAL)", "", match.group(2))
    replacement = f"{match.group(1)}{fields.rstrip()}, aircraft_routing_policy: {policy}{match.group(3)}"
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(text[: match.start()] + replacement + text[match.end() :], encoding="utf-8")


def run_one(cli: Path, label: str, scenario: Path, output: Path, seed: int, policy: str) -> dict:
    output.mkdir(parents=True, exist_ok=True)
    paths = [output / name for name in ("metrics.json", "events.jsonl", "decisions.json")]
    command = [str(cli), "--quiet", "--scenario", str(scenario), "--seed", str(seed),
               "--metrics-json", str(paths[0]), "--record-events", str(paths[1]), "--agent-decisions", str(paths[2])]
    started = time.perf_counter()
    completed = subprocess.run(command, cwd=cli.parent.parent, capture_output=True, text=True)
    runtime_seconds = time.perf_counter() - started
    if completed.returncode:
        raise RuntimeError(f"run failed ({completed.returncode}): {' '.join(command)}\n{completed.stderr}\n{completed.stdout}")
    metrics = json.loads(paths[0].read_text(encoding="utf-8"))
    decisions = json.loads(paths[2].read_text(encoding="utf-8"))["decisions"]
    counterfactual_decisions = [event for event in decisions if event.get("counterfactual")]
    event_records = [json.loads(line) for line in paths[1].read_text(encoding="utf-8").splitlines() if line.strip()]
    normalized_path = output / "normalized.json"
    normalized_path.write_text(json.dumps(normalized(metrics), sort_keys=True, separators=(",", ":")) + "\n", encoding="utf-8")
    completion = {}
    for event in decisions:
        if event["result"] == "OBJECTIVE_COMPLETED":
            completion.setdefault(event["agent_id"], []).append(event["time_seconds"])
    taxi_by_aircraft = {}
    for aircraft in metrics["aircraft_operations"]:
        duration = 0.0
        for prefix in ("arrival", "departure"):
            started_at = aircraft.get(f"{prefix}_taxi_started_at_seconds")
            completed_at = aircraft.get(f"{prefix}_taxi_completed_at_seconds")
            if started_at is not None and completed_at is not None:
                duration += max(0.0, float(completed_at) - float(started_at))
        taxi_by_aircraft[aircraft["flight_number"]] = duration
    return {"scenario": label, "seed": seed, "policy": policy,
            "directory": f"runs/{label}/seed-{seed}/{policy.lower()}", "metrics": metrics,
            "decision_count": len(decisions),
            "replans": metrics["agent_replans"],
            "reservation_contention_waits": sum(event["type"] == "SurfaceWaitingForTraffic" for event in event_records),
            "surface_wait_events": metrics["surface"]["wait_events"],
            "counterfactual_rollouts": sum(event["counterfactual"]["candidate_rollouts"] for event in counterfactual_decisions),
            "counterfactual_candidates_evaluated": sum(event["counterfactual"]["candidate_rollouts"] for event in counterfactual_decisions),
            "counterfactual_simulated_events": sum(event["counterfactual"]["simulated_events"] for event in counterfactual_decisions),
            "different_decisions_from_rolling_horizon": sum(event["counterfactual"]["different_from_rolling_horizon"] for event in counterfactual_decisions),
            "different_decisions_from_goal39": 0,
            "process_runtime_seconds": runtime_seconds,
            "completion_times_seconds": completion,
            "taxi_time_by_aircraft_seconds": taxi_by_aircraft,
            "digests": {path.name: digest(path) for path in [*paths, normalized_path]}}


def normalized(metrics: dict) -> dict:
    return {key: metrics[key] for key in ("seed", "simulated_duration_seconds", "agent_decisions",
            "agent_replans", "agent_objectives_completed", "agent_objectives_failed", "surface", "aircraft_operations")}


def outcome_rank(row: dict) -> tuple:
    metrics = row["metrics"]
    return (metrics["agent_objectives_completed"], -metrics["agent_objectives_failed"],
            -metrics["surface"]["wait_seconds"], -metrics["surface"]["taxi_seconds"],
            -metrics["surface"]["taxi_distance_m"])


def compare_outcome(counterfactual: dict, rolling: dict) -> str:
    left, right = outcome_rank(counterfactual), outcome_rank(rolling)
    return "improved" if left > right else "worsened" if left < right else "no_effect"


def summarize(label: str, policy: str, runs: list[dict]) -> dict:
    metrics = [row["metrics"] for row in runs]
    surfaces = [item["surface"] for item in metrics]
    mean = lambda values: (items := list(values)) and sum(items) / len(items)
    completions = [float(t) for row in runs for times in row["completion_times_seconds"].values() for t in times]
    agent_times = {}
    aircraft_taxi_times = {}
    for row in runs:
        for agent, times in row["completion_times_seconds"].items():
            agent_times.setdefault(agent, []).extend(float(value) for value in times)
        for aircraft, seconds in row["taxi_time_by_aircraft_seconds"].items():
            aircraft_taxi_times.setdefault(aircraft, []).append(seconds)
    return {"scenario": label, "policy": policy, "runs": len(runs),
            "objectives_completed": sum(item["agent_objectives_completed"] for item in metrics),
            "objective_failures": sum(item["agent_objectives_failed"] for item in metrics),
            "mean_taxi_seconds": mean(s["taxi_seconds"] for s in surfaces),
            "total_taxi_seconds": sum(s["taxi_seconds"] for s in surfaces),
            "mean_wait_seconds": mean(s["wait_seconds"] for s in surfaces),
            "total_wait_seconds": sum(s["wait_seconds"] for s in surfaces),
            "mean_route_distance_m": mean(s["taxi_distance_m"] for s in surfaces),
            "total_route_distance_m": sum(s["taxi_distance_m"] for s in surfaces),
            "mean_departure_delay_seconds": mean(m["mean_departure_delay_seconds"] for m in metrics),
            "total_replans": sum(row["replans"] for row in runs),
            "reservation_contention_waits": sum(row["reservation_contention_waits"] for row in runs),
            "surface_wait_events": sum(row["surface_wait_events"] for row in runs),
            "counterfactual_rollouts": sum(row["counterfactual_rollouts"] for row in runs),
            "counterfactual_candidates_evaluated": sum(row["counterfactual_candidates_evaluated"] for row in runs),
            "counterfactual_simulated_events": sum(row["counterfactual_simulated_events"] for row in runs),
            "different_decisions_from_rolling_horizon": sum(row["different_decisions_from_rolling_horizon"] for row in runs),
            "different_decisions_from_goal39": sum(row["different_decisions_from_goal39"] for row in runs),
            "different_decision_rate": sum(row["different_decisions_from_rolling_horizon"] for row in runs) /
                max(1, sum(row["decision_count"] for row in runs)),
            "mean_process_runtime_seconds": mean(row["process_runtime_seconds"] for row in runs),
            "collisions": sum(s["aircraft_aircraft_collisions"] + s["aircraft_ground_collisions"] for s in surfaces),
            "safety_failures": sum(s["safe_failures"] for s in surfaces),
            "minimum_aircraft_separation_m": min(s["minimum_aircraft_separation_m"] for s in surfaces),
            "mean_objective_completion_seconds": mean(completions),
            "mean_objective_completion_seconds_by_agent": {agent: mean(values) for agent, values in sorted(agent_times.items())},
            "mean_taxi_seconds_by_aircraft": {aircraft: mean(values) for aircraft, values in sorted(aircraft_taxi_times.items())},
            "worst_objective_completion_seconds": max(completions, default=0.0),
            "completion_times_by_run": [row["completion_times_seconds"] for row in runs]}


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--output", type=Path, default=None)
    parser.add_argument("--seeds", type=int, nargs="+", default=[42, 43, 44])
    parser.add_argument("--workers", type=int, default=4)
    args = parser.parse_args()
    repo = args.repo.resolve()
    output = (args.output or repo / "docs" / "goal40-evidence").resolve()
    if output.exists() and any(output.iterdir()):
        raise RuntimeError(f"experiment output must be new or empty to prevent stale artifacts: {output}")
    configs = output / "config" / "scenarios"
    output.mkdir(parents=True, exist_ok=True)
    configured = {}
    for label, (filename, _) in MATRIX.items():
        source = repo / "scenarios" / filename
        for policy in POLICIES:
            target = configs / f"{label}-{policy.lower()}.yaml"
            configure_scenario(source, target, policy)
            configured[label, policy] = target
    entries = [(label, seed, policy) for label in MATRIX for seed in args.seeds for policy in POLICIES]
    serial_rows = [run_one(args.cli.resolve(), label, configured[label, policy],
        output / "runs" / label / f"seed-{seed}" / policy.lower(), seed, policy) for label, seed, policy in entries]
    scratch = output / ".parallel-check"
    if scratch.exists():
        shutil.rmtree(scratch)
    parallel_rows = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as executor:
        futures = {executor.submit(run_one, args.cli.resolve(), label, configured[label, policy],
            scratch / label / f"seed-{seed}" / policy.lower(), seed, policy): (label, seed, policy)
            for label, seed, policy in entries}
        for future in concurrent.futures.as_completed(futures):
            parallel_rows[futures[future]] = future.result()
    checks = []
    for row in serial_rows:
        key = row["scenario"], row["seed"], row["policy"]
        replay = parallel_rows[key]
        normalized_match = normalized(row["metrics"]) == normalized(replay["metrics"])
        hash_match = row["digests"] == replay["digests"]
        checks.append({"scenario": key[0], "seed": key[1], "policy": key[2],
            "normalized_outputs_match": normalized_match, "artifact_sha256_match": hash_match,
            "passed": normalized_match and hash_match})
    shutil.rmtree(scratch)
    if not all(item["passed"] for item in checks):
        raise RuntimeError("serial and parallel normalized outputs or artifact hashes differ")
    by_case = {(row["scenario"], row["seed"], row["policy"]): row for row in serial_rows}
    changed_case_outcomes = {"improved": 0, "worsened": 0, "no_effect": 0, "changed_decision_cases": 0}
    for row in serial_rows:
        if row["policy"] not in ("COUNTERFACTUAL", "COST_AWARE_COUNTERFACTUAL"):
            continue
        baseline = by_case[row["scenario"], row["seed"], "ROLLING_HORIZON_COORDINATED"]
        row["outcome_vs_rolling_horizon"] = compare_outcome(row, baseline)
        if row["different_decisions_from_rolling_horizon"] > 0:
            changed_case_outcomes["changed_decision_cases"] += 1
            changed_case_outcomes[row["outcome_vs_rolling_horizon"]] += 1

    # Compare the exact same scenario/seed decisions from Goal 39 and Goal 40.
    for label in MATRIX:
        for seed in args.seeds:
            paths = {
                policy: output / by_case[label, seed, policy]["directory"] / "decisions.json"
                for policy in ("COUNTERFACTUAL", "COST_AWARE_COUNTERFACTUAL")}
            read = lambda path: json.loads(path.read_text(encoding="utf-8"))["decisions"]
            old, new = read(paths["COUNTERFACTUAL"]), read(paths["COST_AWARE_COUNTERFACTUAL"])
            old_by_key = {(item["agent_id"], item["time_seconds"]): item for item in old}
            new_by_key = {(item["agent_id"], item["time_seconds"]): item for item in new}
            differences = 0
            for key in sorted(old_by_key.keys() | new_by_key.keys()):
                before, after = old_by_key.get(key), new_by_key.get(key)
                if before is None or after is None or (before["action"], before.get("selected_route")) != (after["action"], after.get("selected_route")):
                    differences += 1
            by_case[label, seed, "COST_AWARE_COUNTERFACTUAL"]["different_decisions_from_goal39"] = differences
    trace_samples = []
    for row in serial_rows:
        if row["policy"] != "COST_AWARE_COUNTERFACTUAL":
            continue
        decisions_path = output / row["directory"] / "decisions.json"
        decisions = json.loads(decisions_path.read_text(encoding="utf-8"))["decisions"]
        for decision in decisions:
            counterfactual = decision.get("counterfactual")
            if counterfactual and counterfactual["candidate_rollouts"] >= 2:
                trace_samples.append({"scenario": row["scenario"], "seed": row["seed"], "decision": decision})
                if counterfactual["different_from_rolling_horizon"]:
                    break
        if trace_samples and trace_samples[-1]["scenario"] == row["scenario"]:
            break
    table = [summarize(label, policy, [row for row in serial_rows if row["scenario"] == label and row["policy"] == policy])
        for label in MATRIX for policy in POLICIES]
    public_table = [{key: value for key, value in row.items() if key != "completion_times_by_run"} for row in table]
    with (output / "results.csv").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(public_table[0]))
        writer.writeheader()
        writer.writerows(public_table)
    source_scenarios = {label: {"path": f"scenarios/{filename}", "purpose": purpose,
        "sha256": hashlib.sha256((repo / "scenarios" / filename).read_bytes()).hexdigest(),
        "configured": {policy: {"path": f"config/scenarios/{label}-{policy.lower()}.yaml",
            "sha256": digest(configured[label, policy])} for policy in POLICIES}}
        for label, (filename, purpose) in MATRIX.items()}
    aggregate = lambda policy, key: sum(row[key] for row in table if row["policy"] == policy) / len(MATRIX)
    comparison = {"scenario_weighting": "equal mean of ten scenario means",
        "goal39_counterfactual": {key: aggregate("COUNTERFACTUAL", key) for key in
            ("mean_wait_seconds", "mean_taxi_seconds", "mean_route_distance_m", "objectives_completed", "objective_failures", "collisions", "safety_failures", "minimum_aircraft_separation_m", "mean_process_runtime_seconds", "counterfactual_candidates_evaluated", "counterfactual_simulated_events")},
        "goal40_cost_aware": {key: aggregate("COST_AWARE_COUNTERFACTUAL", key) for key in
            ("mean_wait_seconds", "mean_taxi_seconds", "mean_route_distance_m", "objectives_completed", "objective_failures", "collisions", "safety_failures", "minimum_aircraft_separation_m", "mean_process_runtime_seconds", "counterfactual_candidates_evaluated", "counterfactual_simulated_events")},
        "decision_differences_from_goal39": sum(row["different_decisions_from_goal39"] for row in serial_rows if row["policy"] == "COST_AWARE_COUNTERFACTUAL"),
        "goal39_decisions": sum(row["decision_count"] for row in serial_rows if row["policy"] == "COUNTERFACTUAL"),
        "absolute_change": {key: aggregate("COST_AWARE_COUNTERFACTUAL", key) - aggregate("COUNTERFACTUAL", key)
            for key in ("mean_wait_seconds", "mean_taxi_seconds", "mean_route_distance_m", "objectives_completed", "objective_failures", "collisions", "safety_failures", "mean_process_runtime_seconds")}}
    experiment = {"schema_version": 1, "experiment": "Goal 40 cost-aware counterfactual operational planning",
        "policy_matrix": list(POLICIES), "scenarios": source_scenarios, "seeds": args.seeds,
        "serial_runs": len(serial_rows), "parallel_replay_runs": len(checks), "workers": args.workers,
        "configuration_delta": "surface_operations.aircraft_routing_policy only",
        "planner_configuration": {"horizon_seconds": 600, "maximum_route_edges_per_candidate": 8,
            "maximum_events_per_candidate": 256, "maximum_candidates": 8,
            "replan_cooldown_seconds": 10, "objective": ["completed_objectives", "blocked_objectives",
                "downstream_wait", "taxi_time", "taxi_distance", "stable_candidate_order"],
            "cost_aware_model": {"weights": {"wait": 0.40, "completion_time": 0.30, "distance": 0.18, "contention": 0.08, "route_change": 0.04},
                "normalization_limits": {"wait_seconds": 600, "completion_seconds": 600, "distance_m": 2000, "contention_seconds": 600},
                "material_improvement_threshold": 0.01, "maximum_regret_ratio": 0.85,
                "wait_age_fairness_boost": 0.50, "priority_fairness_boost": 0.25,
                "fairness_wait_age_seconds": 120, "maximum_priority": 10}},
        "determinism": {"checks": checks, "passed": all(item["passed"] for item in checks)},
        "safety": {"collisions": sum(row["collisions"] for row in table),
            "safety_failures": sum(row["safety_failures"] for row in table),
            "objective_failures": sum(row["objective_failures"] for row in table),
            "minimum_aircraft_separation_m": min(row["minimum_aircraft_separation_m"] for row in table)},
        "counterfactual_policies_vs_rolling_horizon": changed_case_outcomes,
        "goal40_vs_goal39": comparison,
        "timing": {policy: {"mean_process_runtime_seconds": sum(row["process_runtime_seconds"] for row in serial_rows if row["policy"] == policy) /
            max(1, sum(row["policy"] == policy for row in serial_rows))} for policy in POLICIES},
        "results": table, "runs": [{key: value for key, value in row.items() if key != "metrics"} for row in serial_rows]}
    (output / "config" / "experiment_config.json").write_text(json.dumps({key: experiment[key]
        for key in ("schema_version", "policy_matrix", "scenarios", "seeds", "serial_runs", "parallel_replay_runs",
                    "workers", "configuration_delta", "planner_configuration")}, indent=2) + "\n", encoding="utf-8")
    (output / "experiment.json").write_text(json.dumps(experiment, indent=2) + "\n", encoding="utf-8")
    def find_fixture(scenario: str, agent: str, time_seconds: int) -> dict:
        path = output / "runs" / scenario / "seed-42" / "cost_aware_counterfactual" / "decisions.json"
        decisions = json.loads(path.read_text(encoding="utf-8"))["decisions"]
        match = next((item for item in decisions if item["agent_id"] == agent and
            item["time_seconds"] == time_seconds and item.get("cost_aware_model")), None)
        if match is None:
            raise RuntimeError(f"targeted Goal 40 fixture decision missing: {scenario}/{agent}@{time_seconds}")
        return match

    fixtures = {
        "wasteful_detour": {"scenario": "closure_during_execution", "seed": 42,
            "agent_id": "aircraft-2", "time_seconds": 848,
            "purpose": "Reject a 96 m and 96 s longer route that avoids only 33 s of predicted waiting.",
            "decision": find_fixture("closure_during_execution", "aircraft-2", 848)},
        "worthwhile_detour": {"scenario": "individual_route_choice", "seed": 42,
            "agent_id": "aircraft-3", "time_seconds": 578,
            "purpose": "Accept an 80 m and 80 s route increase that avoids 122 s of predicted waiting.",
            "decision": find_fixture("individual_route_choice", "aircraft-3", 578)},
        "fairness_wait_age_priority": {"scenario": "long_corridor_congestion", "seed": 42,
            "agent_id": "aircraft-1", "time_seconds": 801,
            "purpose": "Re-evaluate actual bounded rollout candidates with 120 s wait age and priority 10; policy-level fixture verifies waiting protection.",
            "decision": find_fixture("long_corridor_congestion", "aircraft-1", 801),
            "replay_inputs": {"wait_age_seconds": 120, "objective_priority": 10}}
    }
    (output / "targeted_fixtures.json").write_text(json.dumps({"schema_version": 1,
        "classification": "synthetic simulator fixtures; no real-airport operational claim",
        "fixtures": fixtures}, indent=2) + "\n", encoding="utf-8")
    trace_samples = [{"fixture": name, "scenario": fixture["scenario"], "seed": fixture["seed"],
        "purpose": fixture["purpose"], "decision": fixture["decision"]}
        for name, fixture in fixtures.items()]
    (output / "representative_decision_traces.json").write_text(json.dumps({"schema_version": 1,
        "purpose": "Goal 40 targeted decisions with baseline, candidates, private rollouts, normalized cost terms, gates, selection, and safety constraints.",
        "traces": trace_samples}, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"serial_runs": len(serial_rows), "parallel_replays": len(checks),
        "determinism_passed": experiment["determinism"]["passed"], "safety": experiment["safety"],
        "goal40_vs_goal39": comparison, "output": str(output)}, indent=2))


if __name__ == "__main__":
    main()
