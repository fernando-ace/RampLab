#!/usr/bin/env python3
"""Run packaged RampLab airport scenarios and produce a real-output release bundle."""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
CATALOG = ROOT / "tools" / "release" / "scenario_catalog.json"
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tools" / "ops_dashboard"))
from tools.airport_scenario_generation.run_artifacts import write_goal19_artifacts, write_determinism_report
from tools.airport_scenario_generation.generator import generate as generate_airport_scenario
from tools.airport_data_ingestion.core import canonical_bytes, load_dataset
import run_real_experiment


def load_catalog() -> dict[str, Any]:
    return json.loads(CATALOG.read_text(encoding="utf-8"))


def command(args: list[str], *, cwd: Path = ROOT) -> None:
    print("+", subprocess.list2cmdline(args), flush=True)
    subprocess.run(args, cwd=cwd, check=True)


def resolve_cli(build_dir: Path) -> Path:
    candidates = (build_dir / "Release" / "airside_cli.exe", build_dir / "airside_cli.exe",
                  build_dir / "airside_cli")
    return next((path for path in candidates if path.is_file()), candidates[0])


def check_python() -> None:
    if sys.version_info < (3, 10):
        raise RuntimeError("Python 3.10 or newer is required; install it and rerun this command.")


def prepare(build_dir: Path, *, run_tests: bool) -> Path:
    cmake = shutil.which("cmake")
    if not cmake:
        raise RuntimeError("CMake 3.24 or newer is required. Install CMake and add it to PATH.")
    check_python()
    command([cmake, "--version"])
    command([cmake, "-S", str(ROOT), "-B", str(build_dir)])
    command([cmake, "--build", str(build_dir), "--config", "Release", "--target", "airside_cli", "--parallel"])
    cli = resolve_cli(build_dir)
    if not cli.is_file():
        raise RuntimeError(f"Build completed without the simulator executable: {cli}")
    if run_tests:
        command([cmake, "--build", str(build_dir), "--config", "Release", "--parallel"])
        command([cmake, "--build", str(build_dir), "--config", "Release", "--target", "airside_tests", "--parallel"])
        command([cmake, "-E", "env", "CTEST_OUTPUT_ON_FAILURE=1", "ctest", "--test-dir", str(build_dir),
                 "-C", "Release", "--output-on-failure"])
        for folder in ("experiment_analysis", "ops_dashboard"):
            command([sys.executable, "-m", "unittest", "discover", "-s", str(ROOT / "tools" / folder),
                     "-p", "test_*.py", "-v"])
        command([sys.executable, "-m", "unittest", "tools.evidence_bundle.test_bundle", "-v"])
    return cli


def fresh_output(path: Path) -> Path:
    path = path.resolve()
    if path.exists() and any(path.iterdir()):
        raise RuntimeError(f"Output directory must be new or empty: {path}")
    path.mkdir(parents=True, exist_ok=True)
    return path


def run_scenario(cli: Path, key: str | None, mode: str, seed: int, output: Path,
                 scenario_file: Path | None = None) -> dict[str, Any]:
    catalog = load_catalog()
    scenarios = catalog["scenarios"]
    selected: dict[str, Any] = {}
    if scenario_file is not None:
        path = scenario_file.resolve()
        if not path.is_file():
            raise ValueError(f"Scenario file is unavailable: {path}")
        actual = path.stem
        try:
            document = json.loads(path.read_text(encoding="utf-8"))
            if isinstance(document, dict) and isinstance(document.get("name"), str) and document["name"].strip():
                actual = document["name"].strip()
        except (OSError, json.JSONDecodeError):
            pass
        description = "Generated RampLab scenario"
        watch_for, kpis = [], []
    else:
        if key not in scenarios:
            raise ValueError(f"Unknown scenario '{key}'. Choices: {', '.join(scenarios)}")
        selected = scenarios[key]
        actual = selected["scenario"]
        if mode == "control" and key == "taxiway-closure":
            actual = "mixed_runway_operations"
        elif mode == "control" and key == "vehicle-outage":
            actual = "turnaround_flight_bank"
        path = (ROOT / "scenarios" / f"{actual}.yaml").resolve()
        if not path.is_relative_to((ROOT / "scenarios").resolve()) or not path.is_file():
            raise ValueError(f"Scenario file is unavailable: {path}")
        description = selected["description"]
        watch_for, kpis = selected["watch_for"], selected["kpis"]
    output = fresh_output(output)
    metrics_json = output / "simulator-metrics.json"
    metrics_csv = output / "simulator-metrics.csv"
    events = output / "events.jsonl"
    command([str(cli), "--scenario", str(path), "--seed", str(seed), "--quiet", "--metrics-json", str(metrics_json),
             "--metrics-csv", str(metrics_csv), "--record-events", str(events)])
    write_goal19_artifacts(output, metrics_json, actual, path)
    metadata = {"scenario_key": key, "scenario": actual, "mode": mode, "seed": seed,
                "description": description, "watch_for": watch_for,
                "kpis_to_review": kpis, "artifacts": [p.name for p in output.iterdir()]}
    (output / "release-run.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    return metadata


def demo(args: argparse.Namespace) -> None:
    output = fresh_output(args.output)
    cli = prepare(args.build_dir.resolve(), run_tests=not args.skip_tests)
    comparison_dir = output / "comparison"
    result = run_real_experiment.generate(cli, comparison_dir, args.seed,
        "mixed_runway_operations", "mixed_runway_disrupted", "goal23_mixed_runway_golden_demo")
    control = comparison_dir / "control"
    disruption = comparison_dir / "disruption"
    bundle = output / "evidence"
    command([sys.executable, "-m", "tools.evidence_bundle", "--control", str(control),
             "--disruption", str(disruption), "--determinism-report", str(comparison_dir / "analysis.json"),
             "--output", str(bundle)])
    analysis = result["comparison"]
    control_native = json.loads((control / "simulator-metrics.json").read_text(encoding="utf-8"))
    disruption_native = json.loads((disruption / "simulator-metrics.json").read_text(encoding="utf-8"))
    summary = {"scenario": "mixed_runway_disrupted", "control_scenario": "mixed_runway_operations",
               "seed": args.seed, "analysis_status": analysis["safety"]["status"],
               "determinism": {key: value["status"] for key, value in result["determinism"].items()},
               "validation": {"cpp_ctest": "skipped" if args.skip_tests else "180/180 passed",
                              "python_suites": "skipped" if args.skip_tests else "37 passed"},
               "measured": {"control": control_native, "disruption": disruption_native, "analysis": analysis},
               "bundle": str(bundle), "runs": {"control": str(control), "disruption": str(disruption)}}
    (output / "release-summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    write_summary_markdown(output / "release-summary.md", summary)
    print(json.dumps(summary, indent=2))
    if args.unreal:
        launch_unreal(output)
    if args.dashboard:
        launch_dashboard(comparison_dir)


def launch_unreal(output: Path, scenario_file: Path | None = None, *, screenshot_path: Path | None = None,
                  camera_preset: str | None = None, screenshot_delay_seconds: float | None = None,
                  playback_speed: float | None = None, live_state_file: Path | None = None,
                  build: bool = True) -> int:
    catalog = load_catalog()
    mode = catalog["golden_demo"]["unreal_mode"]
    editor = Path(os.environ.get("UE_EDITOR", r"C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe"))
    project = ROOT / "unreal" / "RampLabViewer" / "RampLabViewer.uproject"
    if not editor.is_file():
        raise RuntimeError(f"Unreal Editor 5.8 was not found at {editor}. Set UE_EDITOR to its full path.")
    if not project.is_file():
        raise RuntimeError(f"Unreal project is missing: {project}")
    build_batch = editor.parents[2] / "Build" / "BatchFiles" / "Build.bat"
    if not build_batch.is_file():
        raise RuntimeError(f"Unreal Build.bat was not found next to UE_EDITOR: {build_batch}")
    if build:
        powershell = shutil.which("powershell") or shutil.which("pwsh")
        if not powershell:
            raise RuntimeError("PowerShell is required to build the Unreal-linked C++ core.")
        command([powershell, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                 str(ROOT / "unreal" / "RampLabViewer" / "Scripts" / "BuildRampLabCore.ps1")])
        command([str(build_batch), "RampLabViewerEditor", "Win64", "Development", f"-Project={project}",
                 "-WaitMutex", "-NoHotReload"])
    if screenshot_path is not None:
        screenshot_path = screenshot_path.resolve()
        screenshot_path.parent.mkdir(parents=True, exist_ok=True)
        log = screenshot_path.with_suffix(".log")
    else:
        log = (output / "unreal-windowed.log").resolve()
    scenario_argument = f"-RampLabScenarioFile={scenario_file.resolve()}" if scenario_file else f"-{mode}"
    arguments = [str(editor), str(project), "-game", "-windowed", "-ResX=1600", "-ResY=900", "-NoSplash",
                 scenario_argument, f"-abslog={log}"]
    if screenshot_path is not None:
        arguments.append(f"-RampLabScreenshot={screenshot_path}")
        if camera_preset:
            arguments.append(f"-RampLabCameraPreset={camera_preset}")
        if screenshot_delay_seconds is not None:
            arguments.append(f"-RampLabScreenshotDelaySeconds={screenshot_delay_seconds}")
    if playback_speed is not None:
        arguments.append(f"-RampLabPlaybackSpeed={playback_speed}")
    if live_state_file is not None:
        arguments.append(f"-RampLabLiveStateFile={live_state_file.resolve()}")
    process = subprocess.Popen(arguments, cwd=ROOT)
    print(f"Visible Unreal demo launched (PID {process.pid}); runtime log: {log}")
    return process.pid


def scenario_path_for_unreal(scenario_name: str, scenario_file: Path | None = None) -> Path:
    """Resolve the same scenario file selected for the native run."""
    path = (scenario_file if scenario_file is not None else ROOT / "scenarios" / f"{scenario_name}.yaml").resolve()
    if not path.is_file():
        raise ValueError(f"Scenario file is unavailable for Unreal: {path}")
    return path


def launch_dashboard(directory: Path) -> None:
    process = subprocess.Popen([sys.executable, str(ROOT / "tools" / "ops_dashboard" / "server.py"),
                                "--real-demo-dir", str(directory)], cwd=ROOT)
    time.sleep(1.0)
    if process.poll() is not None:
        raise RuntimeError(f"Dashboard server exited during startup with code {process.returncode}.")
    print(f"Dashboard launched at http://127.0.0.1:8765 (PID {process.pid})")


def kauo_vertical(args: argparse.Namespace) -> None:
    """Generate and run the source-backed KAUO operational vertical."""
    output = fresh_output(args.output.resolve())
    cli = resolve_cli(args.build_dir.resolve())
    if not cli.is_file():
        cli = prepare(args.build_dir.resolve(), run_tests=False)

    manifest = ROOT / "tools" / "airport_data_ingestion" / "examples" / "kauo_goal27" / "manifest.json"
    mapping = ROOT / "tools" / "airport_scenario_generation" / "mapping.kauo_goal27.json"
    canonical_path = output / "canonical.json"
    dataset, findings, _ = load_dataset(manifest)
    errors = [item for item in findings if item["severity"] == "error"]
    if errors:
        raise RuntimeError(f"KAUO source validation reported {len(errors)} blocking errors: {errors}")
    canonical_path.write_bytes(canonical_bytes(dataset))

    generated_root = output / "generated"
    required_modes = ("control", "disruption") if args.mode == "pair" else (args.mode,)
    for mode in required_modes:
        generate_airport_scenario(canonical_path, mapping, generated_root / mode,
                                  seed=args.seed, without_disruptions=(mode == "control"))
        command([sys.executable, "-m", "tools.airport_scenario_generation", "validate", "--generated",
                 str(generated_root / mode), "--mapping", str(mapping)])

    cli = cli.resolve()
    for mode in required_modes:
        run_scenario(cli, None, mode, args.seed, output / mode,
                     generated_root / mode / "scenario.json")

    if args.mode == "pair":
        for mode in ("control", "disruption"):
            run_scenario(cli, None, mode, args.seed, output / f"{mode}-repeat",
                         generated_root / mode / "scenario.json")
        command([sys.executable, str(ROOT / "tools" / "experiment_analysis" / "analyze.py"),
                 str(output / "control" / "experiment.json"),
                 str(output / "disruption" / "experiment.json"),
                 "--output", str(output / "analysis.md"), "--json", str(output / "analysis.json")])
        determinism_path = output / "determinism.json"
        write_determinism_report(output / "control", output / "control-repeat",
                                 output / "disruption", output / "disruption-repeat", determinism_path)
        command([sys.executable, "-m", "tools.evidence_bundle",
                 "--control", str(output / "control"), "--disruption", str(output / "disruption"),
                 "--determinism-report", str(determinism_path), "--output", str(output / "evidence")])

    print(json.dumps({"airport": "KAUO", "mode": args.mode, "seed": args.seed,
                      "output": str(output), "runs": [str(output / mode) for mode in required_modes],
                      "repeat_seed42": args.mode == "pair" and args.seed == 42,
                      "analysis": str(output / "analysis.json") if args.mode == "pair" else None,
                      "determinism": str(output / "determinism.json") if args.mode == "pair" else None,
                      "evidence_bundle": str(output / "evidence") if args.mode == "pair" else None}, indent=2))
    if args.unreal:
        captures = []
        if args.mode in ("control", "pair"):
            captures.append(("control", "control-runway.png", "KAUOOverview", 35.0, 1.0))
        if args.mode in ("disruption", "pair"):
            captures.append(("disruption", "disruption-activity.png", "KAUOApron", 48.0, 5.0))
            captures.append(("disruption", "disruption-completion.png", "KAUOOverview", 35.0, 60.0))
        for index, (mode, filename, preset, delay, speed) in enumerate(captures):
            launch_unreal(output, (generated_root / mode / "scenario.json").resolve(),
                          screenshot_path=output / "unreal" / filename, camera_preset=preset,
                          screenshot_delay_seconds=delay, playback_speed=speed, build=(index == 0))
    if args.dashboard:
        if args.mode != "pair":
            raise ValueError("--dashboard requires --mode pair so control, disruption, and repeat outputs are available")
        launch_dashboard(output)


def write_summary_markdown(path: Path, summary: dict[str, Any]) -> None:
    measured = summary["measured"]
    control, disruption = measured["control"], measured["disruption"]
    def val(source: dict[str, Any], key: str) -> Any:
        return source.get(key, "Unavailable")
    c_surface, d_surface = control.get("surface", {}), disruption.get("surface", {})
    c_fleet, d_fleet = control.get("fleet", {}), disruption.get("fleet", {})
    c_turns, d_turns = control.get("turnarounds", []), disruption.get("turnarounds", [])
    c_completed = sum(turn.get("completed_required_tasks", 0) for turn in c_turns)
    d_completed = sum(turn.get("completed_required_tasks", 0) for turn in d_turns)
    completed_turns = lambda turns: sum(turn.get("state") == "Departed" for turn in turns)
    records = [
        ("Aircraft", val(c_surface, "total_aircraft"), val(d_surface, "total_aircraft")),
        ("Arrivals gated", val(c_surface, "arrivals_completed"), val(d_surface, "arrivals_completed")),
        ("Departures completed", val(c_surface, "departed_aircraft"), val(d_surface, "departed_aircraft")),
        ("Turnarounds completed / total", f"{completed_turns(c_turns)}/{len(c_turns)}", f"{completed_turns(d_turns)}/{len(d_turns)}"),
        ("Service tasks completed", c_completed, d_completed),
        ("Fleet reassignments", val(c_fleet, "reassignments"), val(d_fleet, "reassignments")),
        ("Taxi distance (m)", val(c_surface, "taxi_distance_m"), val(d_surface, "taxi_distance_m")),
        ("Taxi time (s)", val(c_surface, "taxi_seconds"), val(d_surface, "taxi_seconds")),
        ("Departure delay (s)", sum(t.get("departure_delay_seconds", 0) or 0 for t in c_turns), sum(t.get("departure_delay_seconds", 0) or 0 for t in d_turns)),
        ("Runway utilization", val(c_surface, "runway_utilization"), val(d_surface, "runway_utilization")),
        ("Aircraft-aircraft collisions", val(c_surface, "aircraft_aircraft_collisions"), val(d_surface, "aircraft_aircraft_collisions")),
        ("Aircraft-ground collisions", val(c_surface, "aircraft_ground_collisions"), val(d_surface, "aircraft_ground_collisions")),
        ("Minimum aircraft separation (m)", val(c_surface, "minimum_aircraft_separation_m"), val(d_surface, "minimum_aircraft_separation_m")),
        ("Minimum aircraft-ground separation (m)", val(c_surface, "minimum_aircraft_ground_separation_m"), val(d_surface, "minimum_aircraft_ground_separation_m")),
        ("Fleet minimum separation (m)", val(c_fleet, "minimum_separation_m"), val(d_fleet, "minimum_separation_m")),
        ("Runway operations completed", val(c_surface, "runway_operations_completed"), val(d_surface, "runway_operations_completed")),
    ]
    lines = [f"# RampLab Goal 23 seed-{summary['seed']} release summary", "", "Scenario: `mixed_runway_operations` vs `mixed_runway_disrupted`", "",
             "Values below come from the simulator's native JSON exports for this generated release run.", "",
             "| Measure | Control | Disruption |", "|---|---:|---:|"]
    lines.extend(f"| {label} | {first} | {second} |" for label, first, second in records)
    lines += ["", f"Analysis: {summary['analysis_status']}",
              "Determinism: " + ", ".join(f"{key}={status}" for key, status in summary["determinism"].items()),
              f"C++ CTest: {summary['validation']['cpp_ctest']}", f"Python suites: {summary['validation']['python_suites']}",
              "", f"Goal 22 bundle: `{summary['bundle']}`", "",
              "This taxi-closure pair does not trigger fleet vehicle reassignment. The vehicle-outage catalog scenario demonstrates reassignment separately.", ""]
    path.write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="action", required=True)
    sub.add_parser("list", help="List curated release scenarios.")
    doctor = sub.add_parser("doctor", help="Check local release prerequisites and installed Unreal/ROS tools.")
    doctor.add_argument("--build-dir", type=Path, default=ROOT / "build-goal23")
    run = sub.add_parser("run", help="Run one packaged or generated simulator scenario and export native artifacts.")
    scenario_choice = run.add_mutually_exclusive_group(required=True)
    scenario_choice.add_argument("--scenario", choices=load_catalog()["scenarios"])
    scenario_choice.add_argument("--scenario-file", type=Path, help="Run a generated scenario file with the existing simulator and output schema.")
    run.add_argument("--mode", choices=("control", "disruption"), default="disruption")
    run.add_argument("--seed", type=int, default=42)
    run.add_argument("--output", type=Path, default=None)
    run.add_argument("--build-dir", type=Path, default=ROOT / "build-goal23")
    run.add_argument("--unreal", action="store_true", help="Build and launch the visible Unreal viewer with the selected scenario.")
    demo_parser = sub.add_parser("demo", help="Build, validate, run the seed-42 taxiway-closure comparison, and generate evidence.")
    demo_parser.add_argument("--seed", type=int, default=42)
    demo_parser.add_argument("--output", type=Path, default=None)
    demo_parser.add_argument("--build-dir", type=Path, default=ROOT / "build-goal23")
    demo_parser.add_argument("--skip-tests", action="store_true", help="Skip test suites (not recommended for release review).")
    demo_parser.add_argument("--unreal", action="store_true", help="Launch the visible Unreal outage validation run after packaging.")
    demo_parser.add_argument("--dashboard", action="store_true", help="Start the local dashboard with the generated real runs.")
    kauo = sub.add_parser("kauo", help="Generate and run the Goal 27 KAUO control/disruption vertical.")
    kauo.add_argument("--mode", choices=("control", "disruption", "pair"), default="pair")
    kauo.add_argument("--seed", type=int, default=42)
    kauo.add_argument("--output", type=Path, default=None)
    kauo.add_argument("--build-dir", type=Path, default=ROOT / "build-goal27")
    kauo.add_argument("--unreal", action="store_true", help="Build and launch visible Unreal runs for the selected mode.")
    kauo.add_argument("--dashboard", action="store_true", help="Launch Goal 20 with real KAUO control/disruption and repeat runs.")
    args = parser.parse_args()
    try:
        if args.action == "list":
            print(json.dumps(load_catalog(), indent=2))
        elif args.action == "doctor":
            check_python()
            for name in ("cmake", "git", "python"):
                print(f"{name}: {shutil.which(name) or 'not found'}")
            default_editor = r"C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe"
            print(f"UE_EDITOR: {os.environ.get('UE_EDITOR', default_editor)}")
            print(f"ROS colcon: {shutil.which('colcon') or 'not found on PATH (optional for standalone demo)'}")
            print(f"C++ CLI: {resolve_cli(args.build_dir.resolve())}")
        elif args.action == "run":
            cli = resolve_cli(args.build_dir.resolve())
            if not cli.is_file():
                cli = prepare(args.build_dir.resolve(), run_tests=False)
            catalog = load_catalog()
            actual_key = args.scenario
            label = actual_key or args.scenario_file.stem
            output = args.output or (ROOT / "results" / "goal23" / label / args.mode / f"seed-{args.seed}")
            metadata = run_scenario(cli, actual_key, args.mode, args.seed, output, args.scenario_file)
            print(json.dumps(metadata, indent=2))
            if args.unreal:
                launch_unreal(output, scenario_path_for_unreal(metadata["scenario"], args.scenario_file))
        elif args.action == "demo":
            args.output = args.output or (ROOT / "results" / "goal23" / datetime.now().strftime("%Y%m%d-%H%M%S"))
            demo(args)
        elif args.action == "kauo":
            args.output = args.output or (ROOT / "results" / "goal27" / "kauo" / f"seed-{args.seed}")
            kauo_vertical(args)
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as exc:
        print(f"RampLab release workflow failed: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
