#!/usr/bin/env python3
"""Build and verify a portable Goal 38 evidence bundle with a SHA-256 ledger."""
from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import zipfile
from pathlib import Path


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--evidence-root", type=Path, default=Path("docs/goal38-evidence"))
    parser.add_argument("--output", type=Path, default=Path("docs/goal38-evidence/portable-bundle"))
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[2]
    source = (repo / args.evidence_root).resolve()
    target = (repo / args.output).resolve()
    if target == source or source not in target.parents:
        raise ValueError("bundle output must be a child of the evidence root")
    experiment = json.loads((source / "experiment.json").read_text(encoding="utf-8"))
    if not experiment["determinism"]["passed"]:
        raise RuntimeError("cannot package failed determinism evidence")
    summary = {
        "experiment": experiment["experiment"], "scenario_count": len(experiment["scenarios"]),
        "policy_matrix": experiment["policy_matrix"], "seeds": experiment["seeds"],
        "serial_runs": experiment["serial_runs"], "parallel_replay_runs": experiment["parallel_replay_runs"],
        "determinism": experiment["determinism"]["passed"], "safety": experiment["safety"],
        "results": [{key: row[key] for key in ("scenario", "policy", "mean_taxi_seconds", "total_taxi_seconds",
            "mean_wait_seconds", "total_wait_seconds", "mean_route_distance_m", "total_route_distance_m",
            "mean_departure_delay_seconds", "objectives_completed", "objective_failures", "total_replans",
            "reservation_contention_waits", "surface_wait_events", "minimum_aircraft_separation_m", "collisions", "safety_failures")}
            | {"mean_objective_completion_seconds_by_agent": row["mean_objective_completion_seconds_by_agent"]}
            for row in experiment["results"]],
    }
    if target.exists():
        shutil.rmtree(target)
    target.mkdir(parents=True)
    for name in ("config", "runs"):
        shutil.copytree(source / name, target / name)
    for name in ("experiment.json", "results.csv"):
        shutil.copy2(source / name, target / name)
    (target / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    policy_rows = {policy: [row for row in experiment["results"] if row["policy"] == policy]
        for policy in experiment["policy_matrix"]}
    lines = ["# Goal 38 portable evidence bundle", "",
        "This bundle contains controlled synthetic simulator evidence. It does not represent measured airport operations.", "",
        f"- Scenarios: {len(experiment['scenarios'])}", f"- Seeds: {', '.join(map(str, experiment['seeds']))}",
        f"- Serial runs: {experiment['serial_runs']}", f"- Parallel replay runs: {experiment['parallel_replay_runs']}",
        f"- Determinism: {'PASS' if experiment['determinism']['passed'] else 'FAIL'}",
        f"- Collisions: {experiment['safety']['collisions']}; safety failures: {experiment['safety']['safety_failures']}; objective failures: {experiment['safety']['objective_failures']}",
        f"- Minimum aircraft separation: {experiment['safety']['minimum_aircraft_separation_m']:.3f} m", "",
        "## Scenario and policy results", "",
        "Each row summarizes three seeds. Mean taxi and wait are seconds per run; distances are metres per run.", "",
        "| Scenario | Policy | Mean taxi s | Mean wait s | Mean distance m | Objectives complete | Failures | Replans | Surface wait events | Reservation waits |",
        "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|"]
    for row in experiment["results"]:
        lines.append(f"| {row['scenario']} | {row['policy']} | {row['mean_taxi_seconds']:.2f} | {row['mean_wait_seconds']:.2f} | {row['mean_route_distance_m']:.2f} | {row['objectives_completed']} | {row['objective_failures']} | {row['total_replans']} | {row['surface_wait_events']} | {row['reservation_contention_waits']} |")
    lines += ["", "## Fleet comparison", "", "Mean of per-scenario means (each scenario is weighted equally):", "",
        "| Policy | Taxi s | Wait s | Distance m | Departure delay s |", "|---|---:|---:|---:|---:|"]
    for policy, rows in policy_rows.items():
        avg = lambda key: sum(row[key] for row in rows) / len(rows)
        lines.append(f"| {policy} | {avg('mean_taxi_seconds'):.2f} | {avg('mean_wait_seconds'):.2f} | {avg('mean_route_distance_m'):.2f} | {avg('mean_departure_delay_seconds'):.2f} |")
    lines += ["", "Differences are descriptive over three fixed seeds. Treat small or inconsistent changes as operationally inconclusive; the matrix does not establish statistical significance.", "",
        "## Files", "", "- `experiment.json` and `summary.json`: inputs, configuration, aggregate results, safety, and replay checks.",
        "- `results.csv`: scenario by policy metrics.", "- `config/scenarios/`: exact policy configured scenario inputs.",
        "- `runs/`: normalized metrics, ordered event records, and structured decisions for every serial run.",
        "- `SHA256SUMS.txt`: SHA-256 integrity ledger for all bundle files except the ledger itself.",
        "- `goal38-portable-evidence.zip`: ZIP archive in the parent evidence folder.", ""]
    (target / "README.md").write_text("\n".join(lines), encoding="utf-8")
    hashes = []
    for path in sorted(target.rglob("*")):
        if path.is_file():
            hashes.append(f"{digest(path)}  {path.relative_to(target).as_posix()}")
    (target / "SHA256SUMS.txt").write_text("\n".join(hashes) + "\n", encoding="utf-8")
    for line in (target / "SHA256SUMS.txt").read_text(encoding="utf-8").splitlines():
        expected, relative = line.split("  ", 1)
        if digest(target / relative) != expected:
            raise RuntimeError(f"hash verification failed: {relative}")
    archive = target.parent / "goal38-portable-evidence.zip"
    if archive.exists():
        archive.unlink()
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as bundle:
        for path in sorted(target.rglob("*")):
            if path.is_file():
                bundle.write(path, Path("goal38-portable-evidence") / path.relative_to(target))
    print(json.dumps({"bundle_directory": str(target), "archive": str(archive),
        "hashed_files": len(hashes), "determinism_passed": experiment["determinism"]["passed"]}, indent=2))


if __name__ == "__main__":
    main()
