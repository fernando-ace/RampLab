#!/usr/bin/env python3
"""Local-only HTTP server for the RampLab operator experiment dashboard."""

from __future__ import annotations

import importlib.util
import json
import mimetypes
import sys
import tempfile
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import unquote, urlparse

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parents[1]
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
        if path == "/api/health":
            return self._send(200, b'{"status":"ok"}', "application/json; charset=utf-8")
        relative = "index.html" if path in {"/", "/index.html"} else path.lstrip("/")
        target = (ROOT / relative).resolve()
        if ROOT not in target.parents and target != ROOT:
            return self._send(403, b"Forbidden", "text/plain; charset=utf-8")
        if not target.is_file():
            return self._send(404, b"Not found", "text/plain; charset=utf-8")
        mime = mimetypes.guess_type(target.name)[0] or "application/octet-stream"
        return self._send(200, target.read_bytes(), f"{mime}; charset=utf-8")

    def do_POST(self):
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
    args = parser.parse_args()
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
