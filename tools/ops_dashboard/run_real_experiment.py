#!/usr/bin/env python3
"""Generate and analyze the canonical simulator-backed Goal 21 experiment."""

from __future__ import annotations

import argparse
import importlib.util
import json
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


def compare_service_routes(control_events: list[dict[str, Any]], disruption_events: list[dict[str, Any]]) -> list[dict[str, Any]]:
    def assignments(events: list[dict[str, Any]]) -> dict[tuple[str, str], list[str]]:
        return {(str(event.get("aircraft_name", "")), str(event.get("service", ""))): event["route_node_names"]
                for event in events if event.get("type") == "VehicleAssigned"
                and isinstance(event.get("route_node_names"), list)}

    control = assignments(control_events)
    disruption = assignments(disruption_events)
    return [{"aircraft": aircraft, "service": service,
             "control_route": control[(aircraft, service)],
             "disruption_route": disruption[(aircraft, service)]}
            for aircraft, service in sorted(set(control) & set(disruption))
            if control[(aircraft, service)] != disruption[(aircraft, service)]]


def generate(cli: Path, output: Path, seed: int) -> dict[str, Any]:
    cli = cli.resolve()
    output.mkdir(parents=True, exist_ok=True)
    specifications = (
        ("control", False), ("control-repeat", False),
        ("disruption", True), ("disruption-repeat", True),
    )
    summaries = {}
    for name, disrupted in specifications:
        destination = output / name
        destination.mkdir(parents=True, exist_ok=True)
        command = [str(cli), "--scenario", "scenarios/baseline.yaml", "--seed", str(seed),
                   "--quiet", "--export-run-dir", str(destination)]
        if not disrupted:
            command.append("--disable-road-events")
        subprocess.run(command, cwd=ROOT, check=True, capture_output=True, text=True)
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
    route_changes = compare_service_routes(control_events.events, disruption_events.events)

    report = ANALYZER.markdown(comparison, control, disruption)
    report += "\n## Service vehicle routes observed in event history\n\n"
    if route_changes:
        report += "Matched `VehicleAssigned` records use different recorded routes between the control and closure run:\n\n"
        for change in route_changes:
            report += (f"- {change['aircraft']} · {change['service']}: "
                       f"`{' -> '.join(change['control_route'])}` → `{' -> '.join(change['disruption_route'])}`\n")
    else:
        report += "No matched service assignment route differed between the recorded runs.\n"
    report += "\n## Repeated-run determinism\n\n"
    for label, result in (("Control metrics", control_determinism), ("Disruption metrics", disruption_determinism),
                          ("Control ordered events", control_event_determinism),
                          ("Disruption ordered events", disruption_event_determinism)):
        report += f"- {label}: **{result['status']}** — {result['message']}\n"
    result = {"schema_version": 1, "comparison": comparison,
              "determinism": {"control": control_determinism, "disruption": disruption_determinism,
                              "control_events": control_event_determinism,
                              "disruption_events": disruption_event_determinism},
              "event_comparison": {"changed_service_assignment_routes": route_changes},
              "bundle_validation": summaries}
    (output / "analysis.md").write_text(report, encoding="utf-8")
    (output / "analysis.json").write_text(json.dumps(result, indent=2, allow_nan=False) + "\n", encoding="utf-8")
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
