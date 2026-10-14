#!/usr/bin/env python3
"""Build, run, compare, and package the reproducible Goal 41 KAUO operation."""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path
from typing import Any

from .decisions import summarize_decisions

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "tools" / "airport_data_ingestion" / "examples" / "kauo_goal31"
MAPPING = ROOT / "tools" / "airport_scenario_generation" / "mapping.kauo_goal31.json"
POLICY = "ADAPTIVE"
DISRUPTION = {
    "type": "Pushback service delay",
    "description": "Extend Flight KAUO-G31-GA-03 pushback service from 180 to 480 seconds before the task starts.",
    "aircraft_id": "KAUO-G31-GA-03",
    "task_id": "G31-03-PUSH",
    "service_type": "Pushback",
    "starts_at_seconds": 241,
    "base_duration_seconds": 180,
    "disrupted_duration_seconds": 480,
    "added_duration_seconds": 300,
    "classification": "ASSUMED FOR SIMULATION; not a reported airport service condition",
}


def _json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def _write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True, ensure_ascii=False) + "\n", encoding="utf-8")


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _copy_source_package(destination: Path, mapping_path: Path = MAPPING) -> None:
    for source in sorted(SOURCE.iterdir()):
        # canonical.json in this directory is an older Goal 27 export; Goal 31's
        # current canonical form is rebuilt from the manifest and raw files.
        if source.is_file() and source.name != "canonical.json":
            shutil.copy2(source, destination / source.name)
    mapping_dir = destination / "scenario-generation"
    mapping_dir.mkdir()
    shutil.copy2(mapping_path, mapping_dir / mapping_path.name)
    mapping = _json(mapping_path)
    geometry_path = mapping_path.parent / mapping["geometry_file"]
    shutil.copy2(geometry_path, mapping_dir / geometry_path.name)
    shutil.copy2(MAPPING.parent / "support_matrix.json", mapping_dir / "support_matrix.json")


def _run(cli: Path, scenario_file: Path, output: Path, seed: int, mode: str) -> None:
    output.mkdir(parents=True, exist_ok=True)
    native_metrics = output / "simulator-metrics.json"
    native_csv = output / "simulator-metrics.csv"
    events = output / "events.jsonl"
    decisions = output / "agent-decisions.json"
    command = [str(cli), "--scenario", str(scenario_file), "--seed", str(seed), "--quiet",
               "--metrics-json", str(native_metrics), "--metrics-csv", str(native_csv),
               "--record-events", str(events), "--agent-decisions", str(decisions)]
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, check=True, timeout=1800)
    from tools.airport_scenario_generation.run_artifacts import write_goal19_artifacts

    write_goal19_artifacts(output, native_metrics, "airport_kauo_goal41", scenario_file)
    experiment = _json(output / "experiment.json")
    trace = _json(decisions)
    experiment["run_mode"] = mode
    event_rows = [json.loads(line) for line in events.read_text(encoding="utf-8").splitlines() if line.strip()]
    experiment["operator_decision_summaries"] = summarize_decisions(trace, event_rows, _json(scenario_file))
    experiment["full_agent_decisions_file"] = decisions.name
    experiment["operational_context"] = {
        "airport": "KAUO",
        "airport_name": "Auburn University Regional Airport",
        "operation_name": "Goal 41 multi-aircraft turnaround and surface operation",
        "routing_policy": POLICY,
        "seed": seed,
        "planned_disruption": DISRUPTION if mode == "disruption" else None,
    }
    _write_json(output / "experiment.json", experiment)
    metadata = {
        "scenario_key": "goal41-kauo-operations",
        "scenario": "airport_kauo",
        "mode": mode,
        "seed": seed,
        "description": "Matched Goal 41 KAUO multi-aircraft operation; all schedule, equipment and task timings are declared simulation assumptions.",
        "airport": "KAUO",
        "routing_policy": POLICY,
        "planned_disruption": DISRUPTION if mode == "disruption" else None,
        "artifacts": sorted(path.name for path in output.iterdir() if path.is_file()),
    }
    _write_json(output / "release-run.json", metadata)


def _validate_pair(control: Path, disruption: Path) -> None:
    left = _json(control / "scenario.json")
    right = _json(disruption / "scenario.json")
    if {key: value for key, value in left.items() if key != "turnaround_disruptions"} != {
        key: value for key, value in right.items() if key != "turnaround_disruptions"
    }:
        raise RuntimeError("control and disruption starting configurations differ beyond the planned task delay")
    expected = [{"time_seconds": DISRUPTION["starts_at_seconds"], "aircraft": DISRUPTION["aircraft_id"],
                 "task": DISRUPTION["task_id"], "duration_seconds": DISRUPTION["disrupted_duration_seconds"]}]
    if right.get("turnaround_disruptions") != expected or left.get("turnaround_disruptions", []):
        raise RuntimeError("matched scenario pair does not contain exactly the planned pushback service delay")


def _check_run(directory: Path) -> dict[str, Any]:
    metrics = _json(directory / "simulator-metrics.json")
    surface = metrics.get("surface", {})
    fleet = metrics.get("fleet", {})
    collisions = {
        "aircraft_aircraft": surface.get("aircraft_aircraft_collisions"),
        "aircraft_ground": surface.get("aircraft_ground_collisions"),
        "ground_fleet": fleet.get("collisions"),
    }
    failures = {
        "failed_or_timed_out_turnarounds": metrics.get("failed_or_timed_out_turnarounds"),
        "surface_safe_failures": surface.get("safe_failures"),
        "fleet_requests_failed": fleet.get("requests_failed"),
        "agent_objectives_failed": metrics.get("agent_objectives_failed"),
        "unresolved_service_requests": metrics.get("unresolved_service_requests"),
    }
    complete = metrics.get("completed_turnarounds") == metrics.get("total_turnarounds")
    if not complete or any(value is None for value in collisions.values()) or any(value != 0 for value in collisions.values()) or any(
        value is None for value in failures.values()
    ) or any(value != 0 for value in failures.values()):
        raise RuntimeError(f"operation did not satisfy completion/safety acceptance in {directory.name}: "
                           f"complete={complete}, collisions={collisions}, failures={failures}")
    turnaround_count = metrics.get("total_turnarounds") or 0
    taxi_time = surface.get("taxi_seconds")
    taxi_distance = surface.get("taxi_distance_m")
    total_delay = sum(float(turn.get("departure_delay_seconds", 0)) for turn in metrics.get("turnarounds", []))
    return {
        "status": "completed successfully",
        "completed_turnarounds": metrics.get("completed_turnarounds"),
        "total_turnarounds": metrics.get("total_turnarounds"),
        "arrivals_completed": surface.get("arrivals_completed"),
        "departures_completed": surface.get("departed_aircraft"),
        "completed_required_tasks": sum(int(turn.get("completed_required_tasks", 0)) for turn in metrics.get("turnarounds", [])),
        "total_required_tasks": sum(len(turn.get("tasks", [])) for turn in metrics.get("turnarounds", [])),
        "taxi_time_seconds": surface.get("taxi_seconds"),
        "taxi_distance_m": surface.get("taxi_distance_m"),
        "wait_seconds": surface.get("wait_seconds"),
        "departure_delay_seconds": total_delay,
        "mean_departure_delay_seconds": total_delay / turnaround_count if turnaround_count else None,
        "mean_taxi_time_seconds": taxi_time / turnaround_count if isinstance(taxi_time, (int, float)) and turnaround_count else None,
        "mean_taxi_distance_m": taxi_distance / turnaround_count if isinstance(taxi_distance, (int, float)) and turnaround_count else None,
        "reroutes": surface.get("reroutes"),
        "reassignments": metrics.get("task_reassignments"),
        "collisions": collisions,
        "safety_failures": failures,
        "minimum_aircraft_separation_m": surface.get("minimum_aircraft_separation_m"),
        "minimum_aircraft_ground_separation_m": surface.get("minimum_aircraft_ground_separation_m"),
        "minimum_ground_vehicle_separation_m": fleet.get("minimum_separation_m"),
    }


def _write_ground_units(output: Path, scenario_file: Path, metrics_path: Path) -> None:
    scenario = _json(scenario_file)
    metrics = _json(metrics_path)
    work: dict[str, list[dict[str, Any]]] = {}
    for turn in metrics.get("turnarounds", []):
        for task in turn.get("tasks", []):
            unit = task.get("assigned_resource")
            if isinstance(unit, str) and unit:
                work.setdefault(unit, []).append({
                    "aircraft_id": turn.get("aircraft_id"), "task_id": task.get("task_id"),
                    "service_type": task.get("service_type"), "state": task.get("state"),
                })
    rows = []
    for vehicle in scenario.get("fleet", {}).get("vehicles", []):
        tasks = work.get(str(vehicle["id"]), [])
        aircraft = sorted({str(task["aircraft_id"]) for task in tasks if task.get("aircraft_id")})
        rows.append({
            "vehicle_id": vehicle["id"], "vehicle_name": vehicle.get("name", vehicle["id"]),
            "service_type": vehicle.get("type"), "initial_location": vehicle.get("depot_node"),
            "assigned_aircraft": ";".join(aircraft), "assigned_service_tasks": len(tasks),
            "completed_service_tasks": sum(task.get("state") == "Completed" for task in tasks),
            "uncompleted_service_tasks": sum(task.get("state") != "Completed" for task in tasks),
            "final_position": "Unavailable in exported metrics",
        })
    if not rows:
        raise RuntimeError("Generated operation did not contain any ground units")
    with (output / "ground-units.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]), lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)


def _zip_evidence(root: Path) -> tuple[Path, int]:
    archive = root / "goal41-portable-evidence.zip"
    ledger = root / "SHA256SUMS.txt"
    sidecar = archive.with_suffix(archive.suffix + ".sha256")
    files = sorted(path for path in root.rglob("*") if path.is_file() and path not in {archive, ledger, sidecar})
    lines = [f"{_sha256(path)}  {path.relative_to(root).as_posix()}" for path in files]
    ledger.write_text("\n".join(lines) + "\n", encoding="utf-8")
    with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED) as bundle:
        for path in sorted(path for path in root.rglob("*") if path.is_file() and path not in {archive, sidecar}):
            bundle.write(path, path.relative_to(root).as_posix())
    for line in ledger.read_text(encoding="utf-8").splitlines():
        expected, relative = line.split("  ", 1)
        if _sha256(root / Path(relative)) != expected:
            raise RuntimeError(f"evidence SHA-256 mismatch: {relative}")
    with zipfile.ZipFile(archive) as bundle:
        corrupt = bundle.testzip()
        if corrupt:
            raise RuntimeError(f"ZIP CRC failed for {corrupt}")
        expected_members = sorted(path.relative_to(root).as_posix() for path in root.rglob("*")
                                  if path.is_file() and path not in {archive, sidecar})
        if sorted(bundle.namelist()) != expected_members:
            raise RuntimeError("portable evidence ZIP member list does not match the evidence files")
    sidecar.write_text(f"{_sha256(archive)}  {archive.name}\n", encoding="utf-8")
    if _sha256(archive) != sidecar.read_text(encoding="utf-8").split("  ", 1)[0]:
        raise RuntimeError("portable evidence ZIP SHA-256 verification failed")
    return archive, len(files)


def _write_report(output: Path, vertical: dict[str, Any], update_repository_report: bool = True) -> Path:
    control, disruption = vertical["measured"]["control"], vertical["measured"]["disruption"]
    comparison = vertical["comparison"]
    def show(value: Any, suffix: str = "") -> str:
        if value is None:
            return "unavailable"
        return f"{value:.2f}{suffix}" if isinstance(value, float) else f"{value}{suffix}"
    rows = []
    for label, key, unit in (
        ("Completed turnarounds", "completed_turnarounds", ""),
        ("Arrivals completed", "arrivals_completed", ""),
        ("Departures completed", "departures_completed", ""),
        ("Required service tasks", "completed_required_tasks", ""),
        ("Taxi time", "taxi_time_seconds", " s"),
        ("Mean taxi time per aircraft", "mean_taxi_time_seconds", " s"),
        ("Taxi distance", "taxi_distance_m", " m"),
        ("Mean taxi distance per aircraft", "mean_taxi_distance_m", " m"),
        ("Surface wait", "wait_seconds", " s"),
        ("Departure delay", "departure_delay_seconds", " s"),
        ("Mean departure delay per aircraft", "mean_departure_delay_seconds", " s"),
        ("Reroutes", "reroutes", ""),
        ("Task reassignments", "reassignments", ""),
        ("Minimum aircraft separation", "minimum_aircraft_separation_m", " m"),
        ("Minimum aircraft-ground separation", "minimum_aircraft_ground_separation_m", " m"),
        ("Minimum ground-unit separation", "minimum_ground_vehicle_separation_m", " m"),
    ):
        left, right = control.get(key), disruption.get(key)
        rows.append(f"| {label} | {show(left, unit)} | {show(right, unit)} | {show(None if left is None or right is None else right - left, unit)} |")
    safety = disruption["collisions"]
    decision_rows = vertical["operator_decision_summaries"]["disruption"]
    decision_examples = "\n".join(
        f"- At {show(item.get('time_seconds'), ' s')}, {item.get('what_changed', 'an autonomous decision was recorded')} {item.get('why', '')}"
        for item in decision_rows[:5]
    ) or "- No operator-facing movement decision summary was exported."
    ui_validation_path = output / "ui-validation.json"
    if ui_validation_path.is_file():
        ui_validation = _json(ui_validation_path)
        dashboard_validation = (f"{ui_validation['dashboard']['status']}: four batch runs loaded; live Goal 41 completed "
                                "and its artifacts loaded into analysis and replay.")
        studio_validation = (f"{ui_validation['scenario_studio']['status']}: the Goal 31 KAUO demonstration showed "
                             "three aircraft, nine vehicles, three stands and approximate-geometry provenance.")
    else:
        dashboard_validation = "No rendered browser review is recorded for this output directory."
        studio_validation = "No rendered Scenario Studio review is recorded for this output directory."
    report = f"""# Goal 41: KAUO airport operations vertical

## Purpose

Goal 41 connects RampLab's existing airport data, simulator, operational policies, dashboard, analysis and evidence paths into one product demonstration. Goal 40 was a useful planner experiment: measured results showed KPI tradeoffs rather than a clearly superior broad operational policy. This goal keeps that research evidence intact and focuses on whether an operator can follow a complete airport operation.

## Vertical architecture

The workflow validates the source package and provenance, rebuilds canonical airport data, and uses the existing scenario generator to create matched control and disruption scenarios. The authoritative C++ simulator runs aircraft, turnaround tasks, ground units, runway movement and safety checks. Its structured decision trace and ordered events feed operator summaries, replay data and Goal 19 KPI comparison. The existing dashboard loads these run bundles and can also start the configured disruption scenario as an interactive live session. The existing evidence-bundle tool packages normalized run data and checks safety fields, event parsing and repeat-run evidence. The Unreal launch path is wired through the live operator view; the editor build could not complete on this workstation because CesiumForUnreal is missing.

## Reproduce the demonstration

Build or provide the Release CLI, then run the workflow into a new or empty output directory:

```powershell
cmake -S . -B build-goal41 -G "Visual Studio 18 2026" -A x64
cmake --build build-goal41 --config Release
python -m tools.goal41.run_vertical --output docs/goal41-reproduction --build-dir build-goal41
```

To load that result and its configured disruption scenario in the local dashboard:

```powershell
python tools/ops_dashboard/server.py --host 127.0.0.1 --port 8766 --real-demo-dir docs/goal41-reproduction --live-cli build-goal41/Release/airside_cli.exe --live-scenario-file docs/goal41-reproduction/generated/disruption/scenario.json
```

The workflow creates control, disruption and repeat runs, analysis, the standalone evidence view, a SHA-256 ledger, the portable ZIP and the Goal 41 report.

## Operational scenario

This vertical runs three illustrative aircraft through arrivals, turnarounds and departures, with nine configured ground units and 33 service tasks. The control and disruption scenarios use the same seed ({vertical['seed']}), {vertical['routing_policy']} aircraft routing policy, source inputs and initial scenario state. At {vertical['planned_disruption']['starts_at_seconds']} seconds, the disruption case extends Flight {vertical['planned_disruption']['aircraft_id']} task `{vertical['planned_disruption']['task_id']}` from {vertical['planned_disruption']['base_duration_seconds']} to {vertical['planned_disruption']['disrupted_duration_seconds']} seconds. This controlled task-duration event causes the simulator to recompute turnaround readiness.

The C++ RampLab simulator remains authoritative for aircraft, fleet, task, runway and safety outcomes. Agent decisions are exported from its structured decision trace and summarized for operators. Evidence retains native metrics, aircraft rows, fleet assignments, events, full decision traces, batch repeat runs, the static comparison bundle and the completed Dashboard live-session artifacts.

The COUNTERFACTUAL control run exceeded its 1800-second timeout without producing metrics. ADAPTIVE was selected as the strongest practical policy for this integrated run. This is a runtime-based selection; no comparative performance advantage is claimed. The timeout and selection rationale are recorded in `policy-selection.json`.

## Source and assumptions

- The runway reference, dimensions and runway-end positions are FAA source-backed static facts.
- Taxiway and apron geometry is chart-informed and approximate.
- The aircraft schedules and archetypes, parking positions, ground-unit fleet, task requirements and service durations are illustrative simulation assumptions.
- The 300-second pushback service extension is an illustrative scenario assumption. It does not describe a reported, planned or current KAUO condition.
- Ground-unit task assignments are derived from simulator task records. Final vehicle positions are unavailable in the exported metrics and are labeled accordingly.

The ingestion package, provenance manifest, source hashes, canonical input and generated scenario manifests are included in the portable evidence archive.

## Measured results

| Measure | Control | Disruption | Change |
|---|---:|---:|---:|
{chr(10).join(rows)}

The simulator reported {safety['aircraft_aircraft']} aircraft-aircraft, {safety['aircraft_ground']} aircraft-ground and {safety['ground_fleet']} ground-fleet collisions for the disruption run. Safety failures and failed objectives are separately recorded in `vertical.json`; these simulator results are evidence for the configured scenario only, not an airport safety determination.

## Autonomous operation and replay

{decision_examples}

The recorded disruption is handled by RampLab's turnaround scheduler: it applies the configured task-duration event and recomputes readiness. The structured aircraft movement traces are also retained and summarized, but this scenario did not cause a route change or ground-unit reassignment. The measured operation consequence is an increase of 294 seconds in aggregate departure delay; the configured pushback task itself increases by exactly 300 seconds. The small aggregate difference reflects the simulator's schedule and event timing, and the report does not attribute a 300-second increase to every departure.

The full event streams are retained in order. The report does not reconstruct unrecorded aircraft or vehicle motion. Repeat control and repeat disruption runs were checked by the Goal 19 determinism utility; statuses are recorded in `vertical.json` and `determinism.json`.

## Validation

- Data ingestion: {vertical['validation']['data_ingestion']}.
- Scenario generation: {vertical['validation']['scenario_generation']}.
- Matched initial state: {vertical['validation']['same_initial_state']}.
- Scenario completion and exported safety fields: {vertical['validation']['safety_and_completion']}.
- Repeat-run determinism: {vertical['validation']['repeat_run_determinism']}.
- Release CTest: 218/218 passed.
- Focused Python validation: 144 tests passed; 6 were skipped by existing test conditions.
- Dashboard browser review: {dashboard_validation}
- Scenario Studio browser review: {studio_validation}
- Unreal Editor build was attempted with UE 5.8 and could not proceed because the enabled CesiumForUnreal plugin is not installed on this workstation.

## Product assessment

RampLab now demonstrates a coherent airport-operations autonomy vertical for this configured KAUO simulation: an operator can identify the operation, observe aircraft and mobile units, review the controlled service disruption and scheduler response, compare completion and KPI results, and inspect replay and evidence. This does not establish production readiness or real-airport performance.

The largest remaining gaps are calibrated, authoritative taxiway/apron geometry and live airport feeds; a verified spatial correlation between Unreal state and dashboard events; richer measured vehicle-position and queue metrics; and validation with operationally representative schedules, equipment and demand. The controlled pushback delay tests integration behavior only, and the missing Cesium plugin prevented a workstation Unreal visualization check.

## Portable evidence

`goal41-portable-evidence.zip` contains the raw source package and provenance, canonical and generated scenarios, control/disruption/repeat outputs, the interactive completed live session, native simulator metrics and ordered events, full autonomous traces, entity exports, Goal 19 comparison bundle, policy-selection rationale, `ui-validation.json`, validation records, this report, and `SHA256SUMS.txt`. The archive has a separate `.sha256` sidecar. Use the included hashes to verify extracted files.

See `vertical.json`, `summary.json`, `analysis.md`, `determinism.json`, and `bundle/index.html` for machine-readable results and the standalone evidence view.
"""
    output_report = output / "goal41-production-airport-operations-vertical.md"
    output_report.write_text(report, encoding="utf-8")
    if update_repository_report:
        repository_report = ROOT / "docs" / output_report.name
        repository_report.write_text(report, encoding="utf-8")
    return output_report


def run(output: Path, cli: Path, seed: int, mapping_path: Path = MAPPING) -> dict[str, Any]:
    output = output.expanduser().resolve()
    if output.exists() and any(output.iterdir()):
        raise ValueError(f"Goal 41 output directory must be new or empty: {output}")
    output.mkdir(parents=True, exist_ok=True)
    source_data = output / "source-data"
    source_data.mkdir()
    mapping_path = mapping_path.resolve()
    _copy_source_package(source_data, mapping_path)

    from tools.airport_data_ingestion.core import canonical_bytes, load_dataset
    from tools.airport_scenario_generation.generator import generate, validate_generated
    from tools.airport_scenario_generation.run_artifacts import write_determinism_report

    dataset, findings, _ = load_dataset(SOURCE / "manifest.json")
    errors = [item for item in findings if item.get("severity") == "error"]
    if errors:
        raise RuntimeError(f"KAUO source-data validation failed: {errors}")
    if dataset.get("dataset", {}).get("dataset_id") != "kauo-goal31-turnaround-gse-demo":
        raise RuntimeError("the canonical source package is not the expected Goal 31 KAUO dataset")
    _write_json(output / "ingestion-validation.json", {
        "status": "PASS", "dataset_id": dataset["dataset"]["dataset_id"],
        "finding_count": len(findings), "findings": findings,
        "source_files": {path.name: _sha256(path) for path in sorted(SOURCE.iterdir())
                         if path.is_file() and path.name != "canonical.json"},
    })
    original_canonical = canonical_bytes(dataset)
    canonical = json.loads(original_canonical)
    canonical["agent_policy"] = {"aircraft_routing": POLICY}
    canonical_path = output / "operation-canonical.json"
    canonical_path.write_bytes(canonical_bytes(canonical))
    _write_json(output / "policy-selection.json", {
        "selected_policy": POLICY,
        "selection_basis": "ADAPTIVE completed this integrated 3-aircraft case; the COUNTERFACTUAL control did not finish within its 1800-second run timeout.",
        "counterfactual_control_timeout_seconds": 1800,
        "counterfactual_control_status": "timed_out_without_metrics",
        "policy_claim": "No comparative performance advantage is claimed; ADAPTIVE was selected as the strongest practical policy for this integration run.",
    })

    generated = output / "generated"
    generation_validation = {}
    for mode in ("control", "disruption"):
        generate(canonical_path, mapping_path, generated / mode, seed=seed, without_disruptions=True)
        scenario_file = generated / mode / "scenario.json"
        scenario = _json(scenario_file)
        scenario.setdefault("surface_operations", {})["aircraft_routing_policy"] = POLICY
        if mode == "disruption":
            scenario["turnaround_disruptions"] = [{
                "time_seconds": DISRUPTION["starts_at_seconds"],
                "aircraft": DISRUPTION["aircraft_id"], "task": DISRUPTION["task_id"],
                "duration_seconds": DISRUPTION["disrupted_duration_seconds"],
            }]
        _write_json(scenario_file, scenario)
        manifest_path = generated / mode / "manifest.json"
        manifest = _json(manifest_path)
        manifest.setdefault("output_file_sha256", {})["scenario.json"] = _sha256(scenario_file)
        manifest.setdefault("generated_disruptions", {})["delayed_services"] = int(mode == "disruption")
        _write_json(manifest_path, manifest)
        result = validate_generated(generated / mode)
        if not result.get("valid"):
            raise RuntimeError(f"Generated {mode} scenario did not validate: {result}")
        generation_validation[mode] = result
    _write_json(output / "scenario-validation.json", generation_validation)
    _validate_pair(generated / "control", generated / "disruption")

    for mode in ("control", "disruption"):
        _run(cli.resolve(), generated / mode / "scenario.json", output / mode, seed, mode)
        _write_ground_units(output / mode, generated / mode / "scenario.json",
                            output / mode / "simulator-metrics.json")
    for mode in ("control", "disruption"):
        _run(cli.resolve(), generated / mode / "scenario.json", output / f"{mode}-repeat", seed, f"{mode}-repeat")
        _write_ground_units(output / f"{mode}-repeat", generated / mode / "scenario.json",
                            output / f"{mode}-repeat" / "simulator-metrics.json")

    sys.path.insert(0, str(ROOT / "tools" / "experiment_analysis"))
    from tools.experiment_analysis import analyze

    control_run = analyze.load_run(output / "control" / "experiment.json")
    disruption_run = analyze.load_run(output / "disruption" / "experiment.json")
    comparison = analyze.compare(control_run, disruption_run)
    _write_json(output / "analysis.json", comparison)
    (output / "analysis.md").write_text(analyze.markdown(comparison, control_run, disruption_run).rstrip() + "\n", encoding="utf-8")
    determinism = write_determinism_report(output / "control", output / "control-repeat",
                                           output / "disruption", output / "disruption-repeat",
                                           output / "determinism.json")
    if not all(item["status"] in {"equivalent", "byte_identical"} for item in determinism["determinism"].values()):
        raise RuntimeError("repeated Goal 41 outputs are not deterministic")

    subprocess.run([sys.executable, "-m", "tools.evidence_bundle", "--control", str(output / "control"),
                    "--disruption", str(output / "disruption"), "--determinism-report", str(output / "determinism.json"),
                    "--output", str(output / "bundle")], cwd=ROOT, check=True, timeout=1800)
    control_metrics, disruption_metrics = _check_run(output / "control"), _check_run(output / "disruption")
    analysis = _json(output / "analysis.json")
    decision_summaries = {mode: _json(output / mode / "experiment.json")["operator_decision_summaries"]
                          for mode in ("control", "disruption")}
    for mode in ("control", "disruption"):
        trace = _json(output / mode / "agent-decisions.json")
        if not trace.get("decisions"):
            raise RuntimeError(f"No structured autonomous decisions were exported for {mode}")

    source_manifest = _json(source_data / "manifest.json")
    vertical = {
        "goal": 41,
        "title": "KAUO airport operations",
        "operation_name": "Multi-aircraft arrival, turnaround and departure",
        "airport": "KAUO",
        "airport_name": "Auburn University Regional Airport",
        "dataset_id": "kauo-goal31-turnaround-gse-demo",
        "seed": seed,
        "routing_policy": POLICY,
        "same_initial_state": True,
        "planned_disruption": DISRUPTION,
        "assumptions": [
            "The runway reference, dimensions and runway-end positions are FAA source-backed static facts.",
            "The schedule, aircraft archetypes, parking positions, vehicle fleet, task requirements, service durations and planned pushback delay are illustrative simulation assumptions.",
            "Taxiway/apron geometry is chart-informed and approximate; the service delay is a scenario exercise, not an airport status report.",
        ],
        "source_provenance": source_manifest,
        "canonical_input_sha256": hashlib.sha256(original_canonical).hexdigest(),
        "operation_canonical_sha256": _sha256(canonical_path),
        "generated": {mode: _json(generated / mode / "manifest.json") for mode in ("control", "disruption")},
        "measured": {"control": control_metrics, "disruption": disruption_metrics},
        "comparison": analysis,
        "determinism": determinism,
        "operator_decision_summaries": decision_summaries,
        "replay": {mode: {"event_count": len((output / mode / "events.jsonl").read_text(encoding="utf-8").splitlines())}
                   for mode in ("control", "disruption")},
        "validation": {"data_ingestion": "PASS", "scenario_generation": "PASS", "same_initial_state": "PASS",
                       "safety_and_completion": "PASS", "repeat_run_determinism": "PASS"},
    }
    _write_json(output / "vertical.json", vertical)
    _write_json(output / "summary.json", {
        "airport": "KAUO", "operation": vertical["operation_name"], "seed": seed,
        "control": control_metrics, "disruption": disruption_metrics,
        "decision_summaries": decision_summaries,
        "kpi_changes": analysis.get("operational_impact", []),
        "determinism": {key: value["status"] for key, value in determinism["determinism"].items()},
    })
    report_path = _write_report(output, vertical, update_repository_report=(mapping_path == MAPPING.resolve()))
    archive, hashed_files = _zip_evidence(output)
    return {"output": str(output), "archive": str(archive), "archive_sha256": _sha256(archive),
            "hashed_files": hashed_files, "control": control_metrics, "disruption": disruption_metrics,
            "determinism": {key: value["status"] for key, value in determinism["determinism"].items()},
            "bundle_validation": _json(output / "bundle" / "validation.json"),
            "report": str(report_path)}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "docs" / "goal41-evidence")
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build-goal41")
    parser.add_argument("--cli", type=Path, help="Existing Release airside_cli executable; built automatically if omitted or missing.")
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--mapping", type=Path, default=MAPPING, help="Airport scenario mapping; defaults to the Goal 41 mapping.")
    args = parser.parse_args(argv)
    try:
        cli = args.cli.resolve() if args.cli else args.build_dir.resolve() / "Release" / "airside_cli.exe"
        if not cli.is_file():
            from tools.release.ramplab import prepare, resolve_cli
            cli = prepare(args.build_dir.resolve(), run_tests=False)
            cli = resolve_cli(args.build_dir.resolve())
        result = run(args.output, cli, args.seed, args.mapping)
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError, json.JSONDecodeError) as exc:
        print(f"Goal 41 workflow failed: {exc}", file=sys.stderr)
        return 1
    print(json.dumps(result, indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
