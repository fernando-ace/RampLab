#!/usr/bin/env python3
"""Local-only HTTP server for the RampLab operator experiment dashboard."""

from __future__ import annotations

import importlib.util
import json
import mimetypes
import subprocess
import sys
import tempfile
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import unquote, urlparse
try:
    from .live_session import LiveSession, LiveSessionError
    from . import scenario_studio
except ImportError:  # Direct `python tools/ops_dashboard/server.py` launch.
    from live_session import LiveSession, LiveSessionError
    import scenario_studio

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parents[1]
REAL_DEMO_DIR: Path | None = None
LIVE_SESSION = LiveSession()
ANALYZER_PATH = REPO / "tools" / "experiment_analysis" / "analyze.py"
spec = importlib.util.spec_from_file_location("ramplab_experiment_analysis", ANALYZER_PATH)
if spec is None or spec.loader is None:
    raise RuntimeError(f"Goal 19 analyzer not found at {ANALYZER_PATH}")
analyzer = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = analyzer
spec.loader.exec_module(analyzer)


def _safe_name(name: str) -> str:
    # Keep only the leaf name: uploaded paths are data, never server paths.
    return Path(name.replace("\\", "/")).name or "upload"


def _real_demo_label(bundle: Path, fallback: str) -> str:
    """Use run metadata so the real-run picker identifies airport and scenario."""
    try:
        release = json.loads((bundle / "release-run.json").read_text(encoding="utf-8"))
        experiment = json.loads((bundle / "experiment.json").read_text(encoding="utf-8"))
        generation = experiment.get("scenario_generation", {})
        manifest = generation.get("manifest", {}) if isinstance(generation, dict) else {}
        airport = manifest.get("airport", {}) if isinstance(manifest, dict) else {}
        airport_name = airport.get("icao") or airport.get("airport_id")
        mode, seed = release.get("mode"), release.get("seed")
        scenario = release.get("scenario") or experiment.get("source_scenario")
        if isinstance(mode, str) and isinstance(airport_name, str):
            label = f"{mode.title()} · {airport_name}"
            if isinstance(scenario, str) and scenario:
                label += f" · {scenario}"
            if isinstance(seed, int) and not isinstance(seed, bool):
                label += f" · seed {seed}"
            return label
    except (OSError, ValueError, TypeError):
        pass
    return fallback


def _run_data(run: dict, folder: Path) -> dict:
    folder.mkdir(parents=True, exist_ok=True)
    files = run.get("files")
    if not isinstance(files, list) or not files:
        raise ValueError("Add at least one file to each run.")
    written: list[Path] = []
    seen: set[str] = set()
    for item in files:
        if not isinstance(item, dict) or not isinstance(item.get("name"), str):
            continue
        name = _safe_name(item["name"])
        if name.casefold() in seen:
            raise ValueError(f"Duplicate filename in run: {name}")
        seen.add(name.casefold())
        content = item.get("content", "")
        if not isinstance(content, str):
            raise ValueError(f"File content for {name} must be text.")
        path = folder / name
        path.write_text(content, encoding="utf-8")
        written.append(path)

    results: list[tuple[Path, analyzer.Run]] = []
    file_reports = []
    for path in written:
        if path.suffix.lower() not in {".json", ".csv", ".jsonl"}:
            file_reports.append({"name": path.name, "status": "unsupported", "message": "Expected JSON, CSV, or JSONL."})
            continue
        try:
            parsed = analyzer.load_run(path)
            results.append((path, parsed))
            file_reports.append({"name": path.name, "status": "loaded", "message": "Parsed successfully."})
        except (OSError, ValueError, json.JSONDecodeError, analyzer.csv.Error) as exc:
            file_reports.append({"name": path.name, "status": "error", "message": str(exc)})

    candidates = [(p, r) for p, r in results if p.suffix.lower() in {".json", ".csv"}]
    primary_pair = next(((p, r) for p, r in candidates if p.name.casefold() in {"experiment.json", "summary.json", "metrics.json"}), None)
    if primary_pair is None and candidates:
        primary_pair = candidates[0]
    if primary_pair is None:
        primary_pair = next(((p, r) for p, r in results if p.suffix.lower() == ".jsonl"), None)

    if primary_pair is None:
        metrics: dict[str, float] = {}
        identity: dict = {}
        name = str(run.get("label") or "Unnamed run")
        raw: dict = {}
        primary_path = None
    else:
        primary_path, parsed = primary_pair
        metrics = dict(parsed.metrics)
        identity = dict(parsed.identity)
        name = str(run.get("label") or parsed.name)
        raw = parsed.raw
        # Merge available numeric KPIs from associated files via Goal 19's parser.
        for path, other in results:
            if path == primary_path or path.suffix.lower() == ".jsonl":
                continue
            metrics.update(other.metrics)

    events = []
    aircraft = []
    disruptions = []
    for path, parsed in results:
        if path.suffix.lower() == ".jsonl":
            events.extend(event for event in parsed.events if isinstance(event, dict))
        if path.suffix.lower() == ".csv":
            rows = parsed.raw.get("rows", [])
            stem = path.stem.casefold()
            if "aircraft" in stem or "entity" in stem:
                aircraft.extend(rows)
            for row in rows:
                kind = str(row.get("event", row.get("type", ""))).casefold()
                if any(token in kind for token in ("disrupt", "rerout", "reassign", "clos", "fail", "recover", "outage")):
                    disruptions.append(row)
    if not aircraft:
        for run_row in raw.get("runs", []) if isinstance(raw, dict) else []:
            for turn in run_row.get("turnarounds", []) if isinstance(run_row, dict) else []:
                aircraft.append({"flight_number": turn.get("aircraft"), "turnaround_id": turn.get("turnaround_id"),
                                 "turnaround_duration_seconds": turn.get("turnaround_duration_seconds"),
                                 "departure_delay_seconds": turn.get("departure_delay_seconds"),
                                 "completion_seconds": turn.get("completion_seconds"), "schedule_slack_seconds": turn.get("schedule_slack_seconds"),
                                 "task_count": len(turn.get("tasks", [])),
                                 "completed_tasks": sum(1 for task in turn.get("tasks", []) if task.get("state") == "Completed")})
    if not events and isinstance(raw.get("events"), list):
        events = [event for event in raw["events"] if isinstance(event, dict)]

    # RampLab records are ordered by simulation time; sort only when a numeric timestamp exists.
    def event_time(event):
        for key in ("time_seconds", "timestamp_seconds", "timestamp_s", "time", "timestamp"):
            value = event.get(key)
            if isinstance(value, (int, float)) and not isinstance(value, bool):
                return float(value)
        return None

    known_times = [event_time(event) for event in events]
    if events and all(value is not None for value in known_times):
        events = [event for _, event in sorted(enumerate(events), key=lambda pair: (event_time(pair[1]), pair[0]))]
    event_history_available = bool(events)
    summary_run = analyzer.Run(str(primary_path or ""), name, identity, metrics, events, raw)
    summary = analyzer.summarize(summary_run)
    safety_fields = [key for key in metrics if key in analyzer.METRICS and analyzer.METRICS[key].category == "safety"]
    collision_fields = ["fleet_collisions", "surface_aircraft_aircraft_collisions", "surface_aircraft_ground_collisions"]
    collision_metrics_present = any(key in metrics for key in collision_fields)
    if not collision_metrics_present:
        safety = {"status": "unknown", "findings": ["Collision metrics are absent; safety cannot be assessed from the available data."], "available_metrics": safety_fields}
    else:
        collisions = sum(metrics.get(key, 0) for key in collision_fields if key in metrics)
        safety = {"status": "collision_detected" if collisions > 0 else "no_collisions_observed",
                  "findings": [f"{collisions:g} collision(s) recorded."], "available_metrics": safety_fields}

    optional_types = {"json": False, "csv": False, "jsonl": False}
    for path, _ in results:
        optional_types[path.suffix.lower().lstrip(".")] = True
    report = {"id": run.get("id"), "name": name, "identity": identity, "metrics": metrics,
              "metric_catalog": {key: {"label": analyzer.METRICS[key].label, "unit": analyzer.METRICS[key].unit,
                                      "category": analyzer.METRICS[key].category}
                                 for key in metrics if key in analyzer.METRICS},
              "safety": safety, "summary": summary, "events": events, "event_history_available": event_history_available,
              "aircraft": aircraft, "disruptions": disruptions, "files": file_reports,
              "missing_optional": [extension.upper() for extension, found in optional_types.items() if not found],
              "primary_path": str(primary_path) if primary_path else None}
    event_pair = next(((path, parsed) for path, parsed in results if path.suffix.lower() == ".jsonl"), None)
    return {"report": report, "primary": primary_pair[1] if primary_pair else None,
            "primary_path": primary_path, "event_run": event_pair[1] if event_pair else None,
            "folder": folder}


class Handler(BaseHTTPRequestHandler):
    server_version = "RampLabOpsDashboard/1.0"

    def log_message(self, fmt, *args):
        # Stay quiet by default; the browser shows actionable upload diagnostics.
        return

    def _send(self, code: int, payload: bytes, content_type: str):
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        try:
            self.wfile.write(payload)
        except (BrokenPipeError, ConnectionResetError):
            # Browser navigation can cancel an in-flight local file request.
            return

    def do_GET(self):
        path = unquote(urlparse(self.path).path)
        parsed_url = urlparse(self.path)
        if path in {"/", "/studio", "/studio/", "/studio.html"}:
            target = (ROOT / "studio.html").resolve()
            return self._send(200, target.read_bytes(), "text/html; charset=utf-8")
        if path == "/dashboard":
            target = (ROOT / "index.html").resolve()
            return self._send(200, target.read_bytes(), "text/html; charset=utf-8")
        if path == "/api/studio/bootstrap":
            from urllib.parse import parse_qs
            try:
                key = parse_qs(parsed_url.query).get("airport", ["kauo"])[0]
                payload = scenario_studio.bootstrap(key)
                payload["runs"] = scenario_studio.RunManagerSingleton.list()
                return self._send(200, json.dumps(payload, ensure_ascii=False).encode(), "application/json; charset=utf-8")
            except (ValueError, OSError, KeyError, TypeError) as exc:
                return self._send(400, json.dumps({"error": str(exc)}).encode(), "application/json; charset=utf-8")
        if path == "/api/studio/scenarios":
            payload = {"scenarios": scenario_studio.ScenarioStoreSingleton.list()}
            return self._send(200, json.dumps(payload, ensure_ascii=False).encode(), "application/json; charset=utf-8")
        if path.startswith("/api/studio/scenarios/"):
            try:
                saved = scenario_studio.ScenarioStoreSingleton.get(path.rsplit("/", 1)[-1])
                return self._send(200, json.dumps({"scenario": saved}, ensure_ascii=False).encode(), "application/json; charset=utf-8")
            except (ValueError, FileNotFoundError) as exc:
                return self._send(404, json.dumps({"error": str(exc)}).encode(), "application/json; charset=utf-8")
        if path == "/api/studio/runs":
            return self._send(200, json.dumps({"runs": scenario_studio.RunManagerSingleton.list()}, ensure_ascii=False).encode(), "application/json; charset=utf-8")
        if path.startswith("/api/studio/runs/") and path.endswith("/export"):
            try:
                data = scenario_studio.export_run(path.split("/")[-2])
                self.send_response(200)
                self.send_header("Content-Type", "application/zip")
                self.send_header("Content-Disposition", 'attachment; filename="ramplab-run-artifacts.zip"')
                self.send_header("Content-Length", str(len(data)))
                self.send_header("Cache-Control", "no-store")
                self.end_headers()
                self.wfile.write(data)
                return
            except (ValueError, FileNotFoundError, OSError) as exc:
                return self._send(400, json.dumps({"error": str(exc)}).encode(), "application/json; charset=utf-8")
        if path.startswith("/api/studio/runs/"):
            try:
                run = scenario_studio.RunManagerSingleton.get(path.rsplit("/", 1)[-1])
                return self._send(200, json.dumps({"run": run}, ensure_ascii=False).encode(), "application/json; charset=utf-8")
            except (ValueError, FileNotFoundError) as exc:
                return self._send(404, json.dumps({"error": str(exc)}).encode(), "application/json; charset=utf-8")
        if path == "/api/session":
            return self._send(200, json.dumps(LIVE_SESSION.state()).encode(), "application/json; charset=utf-8")
        if path == "/api/session/artifacts":
            try:
                return self._send(200, json.dumps(LIVE_SESSION.artifacts()).encode(), "application/json; charset=utf-8")
            except LiveSessionError as exc:
                return self._send(400, json.dumps({"error": str(exc)}).encode(), "application/json; charset=utf-8")
        if path == "/api/health":
            return self._send(200, b'{"status":"ok"}', "application/json; charset=utf-8")
        if path == "/api/real-demo":
            if REAL_DEMO_DIR is None:
                return self._send(404, b'{"error":"Start server with --real-demo-dir RUN_DIRECTORY."}',
                                  "application/json; charset=utf-8")
            try:
                data = []
                for name, folder in (("Control · mixed runway operations", "control"),
                                     ("Disruption · mixed runway operations", "disruption"),
                                     ("Control repeat · seed 42", "control-repeat"),
                                     ("Disruption repeat · seed 42", "disruption-repeat")):
                    bundle = REAL_DEMO_DIR / folder
                    files = []
                    for filename in ("experiment.json", "runs.csv", "aircraft.csv", "events.jsonl"):
                        candidate = (bundle / filename).resolve()
                        if REAL_DEMO_DIR not in candidate.parents or not candidate.is_file():
                            raise FileNotFoundError(f"Missing {folder}/{filename} in configured run bundles.")
                        files.append({"name": filename, "content": candidate.read_text(encoding="utf-8")})
                    data.append({"id": f"real-{folder}", "label": _real_demo_label(bundle, name), "files": files})
                return self._send(200, json.dumps({"runs": data}).encode("utf-8"), "application/json; charset=utf-8")
            except (OSError, ValueError) as exc:
                return self._send(400, json.dumps({"error": str(exc)}).encode("utf-8"), "application/json; charset=utf-8")
        relative = "index.html" if path in {"/", "/index.html"} else path.lstrip("/")
        target = (ROOT / relative).resolve()
        if ROOT not in target.parents and target != ROOT:
            return self._send(403, b"Forbidden", "text/plain; charset=utf-8")
        if not target.is_file():
            return self._send(404, b"Not found", "text/plain; charset=utf-8")
        mime = mimetypes.guess_type(target.name)[0] or "application/octet-stream"
        return self._send(200, target.read_bytes(), f"{mime}; charset=utf-8")

    def do_POST(self):
        if self.path.startswith("/api/studio/"):
            length = int(self.headers.get("Content-Length", "0"))
            if length <= 0 or length > 2 * 1024 * 1024:
                return self._send(413, b'{"error":"Studio requests must be between 1 byte and 2 MB."}', "application/json; charset=utf-8")
            try:
                body = json.loads(self.rfile.read(length))
                if not isinstance(body, dict):
                    raise ValueError("Studio request must be an object.")
                if self.path == "/api/studio/validate":
                    scenario = body.get("scenario")
                    if not isinstance(scenario, dict):
                        raise ValueError("Choose a scenario before validating it.")
                    payload = scenario_studio.validate_scenario(scenario)
                    baseline = scenario_studio.new_scenario(str(scenario.get("airport_key", "kauo")))
                    payload["changes"] = scenario_studio.scenario_diff(baseline, scenario)
                elif self.path == "/api/studio/save":
                    scenario = body.get("scenario")
                    if not isinstance(scenario, dict):
                        raise ValueError("Choose a scenario before saving it.")
                    payload = {"scenario": scenario_studio.ScenarioStoreSingleton.save(scenario)}
                elif self.path == "/api/studio/run":
                    scenario = body.get("scenario")
                    if not isinstance(scenario, dict):
                        raise ValueError("Choose a scenario before running it.")
                    payload = {"run": scenario_studio.RunManagerSingleton.launch(scenario)}
                elif self.path == "/api/studio/export":
                    scenario = body.get("scenario")
                    if not isinstance(scenario, dict):
                        raise ValueError("Choose a scenario before exporting it.")
                    data = scenario_studio.export_generated(scenario)
                    self.send_response(200)
                    self.send_header("Content-Type", "application/zip")
                    self.send_header("Content-Disposition", 'attachment; filename="ramplab-scenario-files.zip"')
                    self.send_header("Content-Length", str(len(data)))
                    self.send_header("Cache-Control", "no-store")
                    self.end_headers()
                    self.wfile.write(data)
                    return
                elif self.path == "/api/studio/compare":
                    payload = {"comparison": scenario_studio.compare_runs(str(body.get("baseline_id", "")), str(body.get("variant_id", "")))}
                elif self.path == "/api/studio/viewer":
                    payload = scenario_studio.launch_viewer(str(body.get("run_id", "")))
                else:
                    return self._send(404, b'{"error":"Not found"}', "application/json; charset=utf-8")
                return self._send(200, json.dumps(payload, ensure_ascii=False).encode("utf-8"), "application/json; charset=utf-8")
            except subprocess.CalledProcessError as exc:
                message = f"The Unreal viewer build failed with exit code {exc.returncode}. Check the local server output for the missing dependency or build error."
                return self._send(400, json.dumps({"error": message}).encode(), "application/json; charset=utf-8")
            except (ValueError, TypeError, KeyError, OSError, RuntimeError) as exc:
                return self._send(400, json.dumps({"error": str(exc)}).encode("utf-8"), "application/json; charset=utf-8")
        session_actions = {"/api/session/start", "/api/session/pause", "/api/session/resume",
                           "/api/session/reset", "/api/session/speed", "/api/session/intervention",
                           "/api/session/advance", "/api/session/finish"}
        session_actions.add("/api/session/unreal")
        session_actions.add("/api/session/replay")
        if self.path in session_actions:
            try:
                length = int(self.headers.get("Content-Length", "0"))
                if length > 64 * 1024:
                    raise LiveSessionError("Session command is too large.")
                body = json.loads(self.rfile.read(length) or b"{}")
                if not isinstance(body, dict):
                    raise LiveSessionError("Session command must be a JSON object.")
                if self.path.endswith("/start"):
                    state = LIVE_SESSION.start()
                elif self.path.endswith("/pause"):
                    state = LIVE_SESSION.pause()
                elif self.path.endswith("/resume"):
                    state = LIVE_SESSION.resume()
                elif self.path.endswith("/reset"):
                    state = LIVE_SESSION.reset()
                elif self.path.endswith("/speed"):
                    state = LIVE_SESSION.set_speed(body.get("speed"))
                elif self.path.endswith("/advance"):
                    state = LIVE_SESSION.advance()
                elif self.path.endswith("/finish"):
                    state = LIVE_SESSION.finish()
                elif self.path.endswith("/unreal"):
                    state = LIVE_SESSION.launch_unreal()
                elif self.path.endswith("/replay"):
                    state = LIVE_SESSION.replay(body.get("interventions"))
                else:
                    kind, target = body.get("type"), body.get("target")
                    if kind not in {"surface_closure", "equipment_outage"}:
                        raise LiveSessionError("Intervention type must be surface_closure or equipment_outage.")
                    if not isinstance(target, int) or isinstance(target, bool) or target < 1:
                        raise LiveSessionError("Intervention target must be a positive simulator ID.")
                    state = LIVE_SESSION.intervene(kind, target, body.get("available", False) is True)
                return self._send(200, json.dumps(state).encode(), "application/json; charset=utf-8")
            except (LiveSessionError, ValueError, TypeError, OSError, json.JSONDecodeError) as exc:
                return self._send(400, json.dumps({"error": str(exc)}).encode(), "application/json; charset=utf-8")
        if self.path != "/api/analyze":
            return self._send(404, b'{"error":"Not found"}', "application/json; charset=utf-8")
        length = int(self.headers.get("Content-Length", "0"))
        if length <= 0 or length > 64 * 1024 * 1024:
            return self._send(413, b'{"error":"Request must be between 1 byte and 64 MB."}', "application/json; charset=utf-8")
        try:
            body = json.loads(self.rfile.read(length))
            requested = body.get("runs", [])
            if not isinstance(requested, list) or not 1 <= len(requested) <= 2:
                raise ValueError("Load one or two runs at a time.")
            with tempfile.TemporaryDirectory(prefix="ramplab-ops-") as temp:
                root = Path(temp)
                loaded = [_run_data(item, root / f"run-{index + 1}") for index, item in enumerate(requested)]
                payload = {"runs": [item["report"] for item in loaded], "analysis": None, "determinism": None,
                           "event_determinism": None, "markdown": None}
                if len(loaded) == 2 and all(item["primary"] is not None for item in loaded):
                    left, right = loaded[0]["primary"], loaded[1]["primary"]
                    comparison = analyzer.compare(left, right)
                    deterministic = analyzer.determinism(left, right) if body.get("determinism") else None
                    event_deterministic = (analyzer.determinism(loaded[0]["event_run"], loaded[1]["event_run"])
                                           if body.get("determinism") and loaded[0]["event_run"] and loaded[1]["event_run"] else None)
                    if deterministic:
                        comparison["determinism"] = deterministic
                    if event_deterministic:
                        comparison["event_determinism"] = event_deterministic
                    payload["analysis"] = comparison
                    payload["determinism"] = deterministic
                    payload["event_determinism"] = event_deterministic
                    payload["markdown"] = analyzer.markdown(comparison, left, right, deterministic)
                    if event_deterministic:
                        payload["markdown"] += "\n## Event-stream determinism\n\n- " + event_deterministic["message"] + "\n"
                        for change in event_deterministic["changed_records"][:10]:
                            before = json.dumps(change["left"], sort_keys=True, ensure_ascii=False)
                            after = json.dumps(change["right"], sort_keys=True, ensure_ascii=False)
                            payload["markdown"] += f"- Ordered event {change['index'] + 1} differs: `{before}` → `{after}`\n"
                elif len(loaded) == 1 and loaded[0]["primary"] is not None:
                    payload["markdown"] = analyzer.markdown(loaded[0]["report"]["summary"], loaded[0]["primary"])
                output = json.dumps(payload, ensure_ascii=False, allow_nan=False).encode("utf-8")
            return self._send(200, output, "application/json; charset=utf-8")
        except (ValueError, TypeError, json.JSONDecodeError, OSError) as exc:
            output = json.dumps({"error": str(exc)}).encode("utf-8")
            return self._send(400, output, "application/json; charset=utf-8")


def main() -> None:
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1", help="Bind address (default: loopback only).")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--real-demo-dir", type=Path,
                        help="Load generated control/ and disruption/ bundles from this local run directory.")
    parser.add_argument("--live-cli", type=Path, help="Path to Release airside_cli for live KAUO sessions.")
    parser.add_argument("--live-root", type=Path, help="Directory for live KAUO run artifacts.")
    args = parser.parse_args()
    global REAL_DEMO_DIR
    REAL_DEMO_DIR = args.real_demo_dir.resolve() if args.real_demo_dir else None
    global LIVE_SESSION
    LIVE_SESSION = LiveSession(cli=args.live_cli, root=args.live_root)
    server = ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"RampLab Operator Experiment Dashboard: http://{args.host}:{args.port}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
