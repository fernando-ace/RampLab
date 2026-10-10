"""Interactive driver for the authoritative RampLab discrete-event simulator."""

from __future__ import annotations

import json
import os
import subprocess
import tempfile
import threading
import time
from pathlib import Path
from typing import Any

REPO = Path(__file__).resolve().parents[2]
import sys
if str(REPO) not in sys.path:
    sys.path.insert(0, str(REPO))

from tools.airport_scenario_generation.generator import generate
from tools.airport_scenario_generation.run_artifacts import write_goal19_artifacts

PACKAGE = REPO / "tools/airport_data_ingestion/examples/kauo_goal27/canonical.json"
MAPPING = REPO / "tools/airport_scenario_generation/mapping.kauo_goal27.json"


class LiveSessionError(ValueError):
    pass


class LiveSession:
    SPEEDS = {"1x": 1.0, "2x": 2.0, "5x": 5.0, "max": None}

    def __init__(self, cli: Path | None = None, root: Path | None = None):
        candidates = [cli] if cli else [REPO / "build-goal28/Release/airside_cli.exe",
                                        REPO / "build/Release/airside_cli.exe",
                                        REPO / "build/airside_cli.exe"]
        self.cli = next((Path(p).resolve() for p in candidates if p and Path(p).is_file()), None)
        self.root = Path(root) if root else Path(tempfile.mkdtemp(prefix="ramplab-goal28-"))
        self.lock = threading.RLock()
        self.process: subprocess.Popen[str] | None = None
        self.scenario_file: Path | None = None
        self.edge_names: dict[int, str] = {}
        self.vehicle_names: dict[int, str] = {}
        self.output_dir: Path | None = None
        self.worker: threading.Thread | None = None
        self.running = False
        self.speed = "max"
        self.snapshot: dict[str, Any] = {"session_state": "idle", "simulated_time_seconds": 0,
                                         "interventions": [], "aircraft": [], "vehicles": [], "roads": [],
                                         "tasks": [], "events": []}
        self.interventions: list[dict[str, Any]] = []
        self.events: list[dict[str, Any]] = []
        self.generation: dict[str, Any] | None = None

    def state(self) -> dict[str, Any]:
        with self.lock:
            return {**self.snapshot, "session_state": self.snapshot.get("session_state", "idle"),
                    "speed": self.speed, "interventions": list(self.interventions),
                    "output_directory": str(self.output_dir) if self.output_dir else None,
                    "live_state_file": str(self.root / "live-state.json")}

    def _read(self) -> dict[str, Any]:
        if self.process is None or self.process.stdout is None:
            raise LiveSessionError("No live KAUO session is running.")
        line = self.process.stdout.readline()
        if not line:
            code = self.process.poll()
            raise LiveSessionError(f"Simulator session ended unexpectedly (exit code {code}).")
        payload = json.loads(line)
        if "error" in payload:
            raise LiveSessionError(payload["error"])
        payload["session_state"] = self.snapshot.get("session_state", "paused")
        self.events.extend(payload.get("events", []))
        payload["events"] = list(self.events)
        payload["interventions"] = list(self.interventions)
        self.snapshot = payload
        self._write_live_state()
        return payload

    def _command(self, command: str) -> dict[str, Any]:
        if self.process is None or self.process.stdin is None:
            raise LiveSessionError("No live KAUO session is running.")
        self.process.stdin.write(command + "\n")
        self.process.stdin.flush()
        return self._read()

    def _write_live_state(self) -> None:
        self.root.mkdir(parents=True, exist_ok=True)
        path = self.root / "live-state.json"
        temporary = path.with_suffix(".json.tmp")
        temporary.write_text(json.dumps(self.snapshot, sort_keys=True, separators=(",", ":")), encoding="utf-8")
        for attempt in range(100):
            try:
                os.replace(temporary, path)
                return
            except PermissionError:
                if attempt == 99:
                    raise
                time.sleep(0.02)

    def _launch(self) -> None:
        if self.cli is None:
            raise LiveSessionError("Release airside_cli was not found. Build it with CMake before starting a session.")
        if self.scenario_file is None or self.output_dir is None:
            raise LiveSessionError("KAUO scenario generation has not completed.")
        self.output_dir.mkdir(parents=True, exist_ok=True)
        self.process = subprocess.Popen(
            [str(self.cli), "--scenario", str(self.scenario_file), "--seed", "42", "--live-control",
             "--record-events", str(self.output_dir / "events.jsonl"),
             "--metrics-json", str(self.output_dir / "simulator-metrics.json"),
             "--metrics-csv", str(self.output_dir / "simulator-metrics.csv")],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, encoding="utf-8", bufsize=1)
        self.snapshot = self._read()
        self.snapshot["session_state"] = "paused"

    def start(self) -> dict[str, Any]:
        with self.lock:
            self._stop_process()
            self.root.mkdir(parents=True, exist_ok=True)
            generated = self.root / "generated" / "control"
            self.generation = generate(PACKAGE, MAPPING, generated, seed=42, without_disruptions=True)
            self.scenario_file = generated / "scenario.json"
            document = json.loads(self.scenario_file.read_text(encoding="utf-8"))
            self.edge_names = {index + 1: str(edge["id"]) for index, edge in enumerate(document["airport"]["edges"])}
            self.vehicle_names = {index + 1: str(vehicle["name"]) for index, vehicle in enumerate(document["fleet"]["vehicles"])}
            self.output_dir = self.root / "run-001"
            self.interventions = []
            self.events = []
            self.running = False
            self.speed = "max"
            self._launch()
            self.snapshot["session_state"] = "paused"
            self._write_live_state()
            return self.state()

    def reset(self) -> dict[str, Any]:
        with self.lock:
            if self.scenario_file is None:
                return self.start()
            self._stop_process()
            attempt = 1
            while (self.root / f"run-{attempt:03d}").exists():
                attempt += 1
            self.output_dir = self.root / f"run-{attempt:03d}"
            self.interventions = []
            self.events = []
            self.running = False
            self.speed = "max"
            self._launch()
            self.snapshot["session_state"] = "paused"
            self._write_live_state()
            return self.state()

    def _stop_process(self) -> None:
        self.running = False
        if self.process is not None:
            if self.process.poll() is None:
                try:
                    if self.process.stdin:
                        self.process.stdin.write("quit\n")
                        self.process.stdin.flush()
                except OSError:
                    pass
                self.process.terminate()
                try:
                    self.process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    self.process.kill()
                    self.process.wait(timeout=2)
            self._close_process_handles()
            self.process = None

    def pause(self) -> dict[str, Any]:
        with self.lock:
            self.running = False
            self.snapshot["session_state"] = "paused"
            self._write_live_state()
            return self.state()

    def resume(self) -> dict[str, Any]:
        with self.lock:
            if self.process is None:
                raise LiveSessionError("Start the KAUO session first.")
            if self.snapshot.get("finished"):
                raise LiveSessionError("The KAUO session has finished; reset it to run again.")
            self.running = True
            self.snapshot["session_state"] = "running"
            self._write_live_state()
            if self.worker is None or not self.worker.is_alive():
                self.worker = threading.Thread(target=self._run, daemon=True)
                self.worker.start()
            return self.state()

    def set_speed(self, speed: str) -> dict[str, Any]:
        if speed not in self.SPEEDS:
            raise LiveSessionError("Speed must be one of 1x, 2x, 5x, or max.")
        with self.lock:
            self.speed = speed
            return self.state()

    def intervene(self, kind: str, target: int, available: bool = False) -> dict[str, Any]:
        with self.lock:
            if self.process is None or self.snapshot.get("finished"):
                raise LiveSessionError("Interventions require an active session.")
            was_running = self.running
            self.running = False
            prior_event_count = len(self.events)
            action = f"closure {target} {1 if available else 0}" if kind == "surface_closure" else f"outage {target}"
            result = self._command(action)
            event = next((item for item in result.get("events", [])[prior_event_count:]
                          if item.get("type") == "OperatorIntervention"), None)
            if event is None:
                raise LiveSessionError("Simulator did not acknowledge the intervention event.")
            entry = {"order": len(self.interventions) + 1, "simulated_time_seconds": event["time_seconds"],
                     "sequence": event["sequence"], "type": kind, "target": target,
                     "target_name": self.edge_names.get(target, str(target)) if kind == "surface_closure"
                     else self.vehicle_names.get(target, str(target)),
                     "result": event.get("detail", "accepted")}
            self.interventions.append(entry)
            self.snapshot["interventions"] = list(self.interventions)
            (self.output_dir / "interventions.json").write_text(
                json.dumps(self.interventions, indent=2, sort_keys=True) + "\n", encoding="utf-8")
            self._write_live_state()
            self.running = was_running
            self.snapshot["session_state"] = "running" if was_running else "paused"
            self._write_live_state()
            return self.state()

    def advance(self) -> dict[str, Any]:
        with self.lock:
            if self.process is None or self.snapshot.get("finished"):
                return self.state()
            return self._command("advance")

    def finish(self) -> dict[str, Any]:
        with self.lock:
            if self.process is None:
                raise LiveSessionError("Start the KAUO session first.")
            self.running = False
            result = self._command("finish")
            result["session_state"] = "completed"
            result["interventions"] = list(self.interventions)
            self.snapshot = result
            self._write_live_state()
            if self.process:
                self.process.wait(timeout=30)
                self._close_process_handles()
                self.process = None
            if self.output_dir and self.scenario_file:
                write_goal19_artifacts(self.output_dir, self.output_dir / "simulator-metrics.json",
                                       "airport_kauo_goal28", self.scenario_file)
            return self.state()

    def replay(self, actions: list[dict[str, Any]]) -> dict[str, Any]:
        if not isinstance(actions, list) or not actions:
            raise LiveSessionError("Replay requires a non-empty intervention list.")
        self.reset()
        self.set_speed("max")
        for ordinal, action in enumerate(actions, 1):
            if not isinstance(action, dict):
                raise LiveSessionError(f"Replay action {ordinal} must be an object.")
            target_time = action.get("simulated_time_seconds")
            kind, target = action.get("type"), action.get("target")
            if not isinstance(target_time, int) or isinstance(target_time, bool) or target_time < 0:
                raise LiveSessionError(f"Replay action {ordinal} has an invalid simulated timestamp.")
            if kind not in {"surface_closure", "equipment_outage"} or not isinstance(target, int) or isinstance(target, bool):
                raise LiveSessionError(f"Replay action {ordinal} has an invalid type or target.")
            while int(self.snapshot.get("simulated_time_seconds", 0)) < target_time:
                if self.snapshot.get("finished"):
                    raise LiveSessionError(f"Session ended before intervention {ordinal} at {target_time}s.")
                self.advance()
            if int(self.snapshot.get("simulated_time_seconds", 0)) != target_time:
                raise LiveSessionError(f"No simulator event boundary exists at replay time {target_time}s.")
            self.intervene(kind, target, False)
        return self.finish()

    def artifacts(self) -> dict[str, Any]:
        with self.lock:
            if not self.output_dir or not (self.output_dir / "simulator-metrics.json").is_file():
                raise LiveSessionError("Finish the live run before loading its analysis artifacts.")
            names = ("experiment.json", "runs.csv", "aircraft.csv", "events.jsonl",
                     "simulator-metrics.json", "simulator-metrics.csv", "interventions.json")
            files = [{"name": name, "content": (self.output_dir / name).read_text(encoding="utf-8")}
                     for name in names if (self.output_dir / name).is_file()]
            return {"run": {"id": f"goal28-{self.output_dir.name}",
                             "label": f"Interactive KAUO · seed 42 · {self.output_dir.name}", "files": files}}

    def launch_unreal(self) -> dict[str, Any]:
        with self.lock:
            if self.scenario_file is None:
                raise LiveSessionError("Start the KAUO session before connecting Unreal.")
            from tools.release.ramplab import launch_unreal
            log = self.root / "unreal"
            log.mkdir(parents=True, exist_ok=True)
            screenshot = self.output_dir / "unreal-live.png" if self.output_dir else log / "unreal-live.png"
            pid = launch_unreal(log, self.scenario_file, live_state_file=self.root / "live-state.json",
                                screenshot_path=screenshot, camera_preset="KAUO Overview",
                                screenshot_delay_seconds=5.0, build=False)
            return {"pid": pid, "state_file": str(self.root / "live-state.json"),
                    "screenshot_path": str(screenshot)}

    def _run(self) -> None:
        while True:
            with self.lock:
                if not self.running or self.process is None or self.snapshot.get("finished"):
                    break
                old_time = float(self.snapshot.get("simulated_time_seconds", 0))
                speed = self.SPEEDS[self.speed]
                try:
                    result = self._command("advance")
                    new_time = float(result.get("simulated_time_seconds", old_time))
                    if result.get("finished"):
                        self.running = False
                        result["session_state"] = "completed"
                    else:
                        result["session_state"] = "running"
                except (LiveSessionError, OSError, ValueError):
                    self.running = False
                    self.snapshot["session_state"] = "error"
                    break
            delta = max(0.0, new_time - old_time)
            if speed is not None and delta:
                time.sleep(min(1.0, delta / speed))

    def close(self) -> None:
        with self.lock:
            self._stop_process()

    def _close_process_handles(self) -> None:
        if self.process is None:
            return
        for handle in (self.process.stdin, self.process.stdout, self.process.stderr):
            if handle is not None:
                handle.close()
