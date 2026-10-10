"""Build the repeatable seed-42 KAUO Goal 28 control/intervention exercise."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.airport_scenario_generation.generator import generate
from tools.airport_scenario_generation.run_artifacts import make_determinism_report, write_goal19_artifacts
from tools.evidence_bundle.bundle import build as build_evidence
from tools.ops_dashboard.live_session import LiveSession


def run_control(cli: Path, scenario_file: Path, output: Path) -> None:
    output.mkdir(parents=True, exist_ok=True)
    subprocess.run([str(cli), "--scenario", str(scenario_file), "--seed", "42", "--quiet",
                    "--record-events", str(output / "events.jsonl"),
                    "--metrics-json", str(output / "simulator-metrics.json"),
                    "--metrics-csv", str(output / "simulator-metrics.csv")], check=True)
    write_goal19_artifacts(output, output / "simulator-metrics.json", "airport_kauo_goal28", scenario_file)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", type=Path, default=ROOT / "build-goal28/Release/airside_cli.exe")
    parser.add_argument("--output", type=Path, default=ROOT / "results/goal28/kauo/seed-42-final")
    parser.add_argument("--unreal", action="store_true", help="Launch the live Unreal mirror and include its screenshot in the bundle.")
    args = parser.parse_args()
    cli, output = args.cli.resolve(), args.output.resolve()
    if not cli.is_file():
        parser.error(f"Release simulator not found: {cli}")
    if output.exists() and any(output.iterdir()):
        parser.error(f"Output directory must be new or empty: {output}")
    output.mkdir(parents=True, exist_ok=True)
    generated = output / "generated" / "control"
    generate(ROOT / "tools/airport_data_ingestion/examples/kauo_goal27/canonical.json",
             ROOT / "tools/airport_scenario_generation/mapping.kauo_goal27.json",
             generated, seed=42, without_disruptions=True)
    scenario_file = generated / "scenario.json"
    run_control(cli, scenario_file, output / "control")
    run_control(cli, scenario_file, output / "control-repeat")

    runtime = LiveSession(cli=cli, root=output / "interactive-runtime")
    screenshot_path: Path | None = None
    try:
        state = runtime.start()
        geometry = json.loads((ROOT / "tools/airport_scenario_generation/kauo_goal27.geometry.json").read_text(encoding="utf-8"))
        closure_edge = next(index + 1 for index, edge in enumerate(geometry["airport"]["edges"])
                            if edge["id"] == "twy_a_mid_fbo_primary")
        scenario = json.loads(runtime.scenario_file.read_text(encoding="utf-8"))
        mobile_unit = next(index + 1 for index, vehicle in enumerate(scenario["fleet"]["vehicles"])
                           if vehicle["type"] == "baggage_load")
        for when, kind, target in ((15, "surface_closure", closure_edge), (2500, "equipment_outage", mobile_unit)):
            while int(state["simulated_time_seconds"]) < when:
                if state.get("finished"):
                    raise RuntimeError(f"KAUO run finished before intervention at {when} seconds")
                state = runtime.advance()
            if int(state["simulated_time_seconds"]) != when:
                raise RuntimeError(f"Simulator has no event boundary at requested time {when} seconds")
            state = runtime.intervene(kind, target)
            if args.unreal and kind == "surface_closure":
                screenshot_path = Path(runtime.launch_unreal()["screenshot_path"])
                deadline = time.monotonic() + 180
                while not screenshot_path.is_file() and time.monotonic() < deadline:
                    time.sleep(0.25)
                if not screenshot_path.is_file():
                    raise RuntimeError(f"Unreal live screenshot was not produced: {screenshot_path}")
        intervention_log = list(runtime.interventions)
        runtime.finish()
        interactive = runtime.output_dir
        if interactive is None:
            raise RuntimeError("Interactive run output directory was lost")
        repeat_state = runtime.replay(intervention_log)
        interactive_repeat = runtime.output_dir
        if interactive_repeat is None:
            raise RuntimeError("Replay output directory was lost")

        compared = ("simulator-metrics.json", "events.jsonl")
        determinism = {
            name: "byte_identical" if (interactive / name).read_bytes() == (interactive_repeat / name).read_bytes()
            else "different" for name in compared
        }
        report = make_determinism_report(output / "control", output / "control-repeat", interactive, interactive_repeat)
        report_path = output / "determinism.json"
        report_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        analysis_command = [sys.executable, str(ROOT / "tools/experiment_analysis/analyze.py"),
                            str(output / "control/experiment.json"), str(interactive / "experiment.json"),
                            "--output", str(output / "analysis.md"), "--json", str(output / "analysis.json")]
        subprocess.run(analysis_command, check=True)
        evidence = build_evidence([("control", output / "control"), ("interactive", interactive)],
                                  output / "evidence", report_path)
        metrics_control = json.loads((output / "control/simulator-metrics.json").read_text(encoding="utf-8"))
        metrics_interactive = json.loads((interactive / "simulator-metrics.json").read_text(encoding="utf-8"))
        summary = {"scenario": "airport_kauo", "seed": 42, "interventions": intervention_log,
                   "interactive_repeat_byte_identity": determinism,
                   "unreal_live_screenshot": str(screenshot_path) if screenshot_path else None,
                   "control_vs_interactive": {"control": metrics_control, "interactive": metrics_interactive},
                   "event_counts": {"control": sum(1 for _ in (output / "control/events.jsonl").open(encoding="utf-8")),
                                    "interactive": sum(1 for _ in (interactive / "events.jsonl").open(encoding="utf-8"))},
                   "evidence_validation": evidence["validation"], "interactive_repeat_simulated_time_seconds": repeat_state["simulated_time_seconds"]}
        (output / "goal28-summary.json").write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        print(json.dumps({"output": str(output), "interventions": intervention_log,
                          "interactive_determinism": determinism,
                          "evidence_status": evidence["validation"]["status"],
                          "control_metrics": metrics_control,
                          "interactive_metrics": metrics_interactive}, indent=2))
        return 0
    finally:
        runtime.close()


if __name__ == "__main__":
    raise SystemExit(main())
