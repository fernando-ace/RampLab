"""Deterministic, resumable experiment orchestration for Scenario Studio.

The runner adapts registered Goal 30 scenario inputs and delegates each run to
the same generator, native CLI, and Goal 19 analysis path as Scenario Studio.
"""
from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor, as_completed
from copy import deepcopy
from datetime import datetime, timedelta
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import platform
import subprocess
import sys
import threading
import time
from typing import Any
import uuid

MAX_RUNS = 100
MAX_WORKERS = 8
SCHEMA_VERSION = 1
RUNNING = {"QUEUED", "RUNNING"}


def canonical(value: Any) -> bytes:
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"), allow_nan=False).encode("utf-8")


def digest(value: Any) -> str:
    return hashlib.sha256(value if isinstance(value, bytes) else canonical(value)).hexdigest()


def registry(scenario: dict[str, Any], studio: Any) -> list[dict[str, Any]]:
    """Return only inputs that Goal 30's scenario generator consumes."""
    entries: list[dict[str, Any]] = []
    entries.append({"id": "agent_policy.aircraft_routing", "name": "Aircraft routing policy",
                    "category": "Autonomy", "adapter": "path", "path": ["agent_policy", "aircraft_routing"],
                    "type": "categorical", "allowed_values": ["SCRIPTED", "ADAPTIVE"], "units": "policy",
                    "restart_required": True, "description": "Selects the deterministic aircraft surface routing policy."})
    for flight in scenario.get("flights", []):
        ident = str(flight.get("flight_id", ""))
        if ident:
            entries.append({"id": f"flight.{ident}.scheduled_time", "name": f"{ident} scheduled time",
                            "category": "Flight timing", "adapter": "path", "path": ["flights", {"flight_id": ident}, "scheduled_time"],
                            "flight_id": ident, "operation": flight.get("operation"),
                            "type": "datetime", "units": "ISO 8601", "restart_required": True,
                            "description": "Scheduled time read by the Goal 24B scenario generator."})
    for requirement in scenario.get("turnaround_requirements", []):
        ident = str(requirement.get("requirement_id", ""))
        if ident:
            entries.append({"id": f"turnaround.{ident}.duration_minutes", "name": f"{ident} service duration",
                            "category": "Turnaround", "adapter": "path", "path": ["turnaround_requirements", {"requirement_id": ident}, "duration_minutes"],
                            "type": "number", "minimum": 0.01, "units": "min", "restart_required": True,
                            "description": "Service duration read by the Goal 24B generator and native turnaround simulation."})
    for equipment in scenario.get("equipment", []):
        ident = str(equipment.get("equipment_id", ""))
        if ident:
            entries.append({"id": f"fleet.{ident}.status", "name": f"{ident} availability", "category": "Fleet",
                            "adapter": "path", "path": ["equipment", {"equipment_id": ident}, "status"],
                            "type": "categorical", "allowed_values": ["available", "unavailable"], "units": "state", "restart_required": True,
                            "description": "Availability status consumed by the Goal 24B mapped fleet generator."})
    route_ids = sorted(studio.load_mapping(studio._mapping(str(scenario.get("airport_key", "kauo"))))[0].get("route_ids", {}))
    if route_ids:
        entries.extend([
            {"id": "disruption.route_closure.start_time", "name": "Route closure start", "category": "Disruption",
             "adapter": "closure_start", "type": "datetime", "units": "ISO 8601", "restart_required": True,
             "route_id": route_ids[0], "description": "Adds or moves a closure on a route explicitly mapped to this dataset."},
            {"id": "disruption.route_closure.duration_minutes", "name": "Route closure duration", "category": "Disruption",
             "adapter": "closure_duration", "type": "number", "units": "min", "restart_required": True,
             "route_id": route_ids[0], "description": "Sets the mapped closure interval consumed by the scenario generator."},
        ])
    return entries


def _axis_values(axis: dict[str, Any]) -> list[Any]:
    if not isinstance(axis, dict):
        raise ValueError("Each parameter needs a definition object.")
    if "values" in axis:
        values = axis["values"]
        if not isinstance(values, list) or not values:
            raise ValueError("Parameter value lists must contain at least one value.")
    elif "range" in axis:
        spec = axis["range"]
        if not isinstance(spec, dict) or set(spec) != {"start", "stop", "step"}:
            raise ValueError("Numeric ranges need start, stop, and step.")
        start, stop, step = spec["start"], spec["stop"], spec["step"]
        if any(isinstance(x, bool) or not isinstance(x, (int, float)) or not math.isfinite(x) for x in (start, stop, step)) or step == 0:
            raise ValueError("Range values must be finite numbers and step cannot be zero.")
        if (stop - start) * step < 0:
            raise ValueError("Range step must move from start toward stop.")
        count = int(math.floor((stop - start) / step + 1e-9)) + 1
        if count < 1 or count > MAX_RUNS:
            raise ValueError("Numeric parameter ranges are limited to 100 values.")
        values = [start + index * step for index in range(count)]
        values = [int(x) if isinstance(start, int) and isinstance(step, int) else x for x in values]
    else:
        raise ValueError("Each parameter needs values or a numeric range.")
    if len({canonical(value) for value in values}) != len(values):
        raise ValueError("Duplicate parameter values are not allowed.")
    return values


def _validate_value(definition: dict[str, Any], value: Any) -> None:
    kind = definition["type"]
    if kind == "integer" and (isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= 2147483647):
        raise ValueError(f"{definition['name']} must be an integer from 0 through 2,147,483,647.")
    if kind == "number" and (isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or value < definition.get("minimum", 0.000001)):
        raise ValueError(f"{definition['name']} must be a positive finite number.")
    if kind == "categorical" and value not in definition.get("allowed_values", []):
        raise ValueError(f"{definition['name']} value must be one of: {', '.join(definition.get('allowed_values', []))}.")
    if kind == "datetime":
        if not isinstance(value, str):
            raise ValueError(f"{definition['name']} values must be ISO 8601 strings.")
        try:
            parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
        except ValueError as exc:
            raise ValueError(f"{definition['name']} needs a valid ISO 8601 value.") from exc
        if parsed.tzinfo is None:
            raise ValueError(f"{definition['name']} needs a UTC offset.")


def _apply_parameters(base: dict[str, Any], parameters: dict[str, Any], definitions: dict[str, dict[str, Any]]) -> dict[str, Any]:
    result = deepcopy(base)
    closure = None
    ordered = sorted(parameters.items(), key=lambda item: 0 if definitions[item[0]]["adapter"] != "closure_duration" else 1)
    for ident, value in ordered:
        definition = definitions[ident]
        adapter = definition["adapter"]
        if adapter == "path":
            target: Any = result
            path = definition.get("path", [])
            if not path: raise ValueError(f"Registered parameter {ident} has no scenario path.")
            for component in path[:-1]:
                if isinstance(component, dict) and isinstance(target, list):
                    target = next((item for item in target if all(item.get(key) == expected for key, expected in component.items())), None)
                elif isinstance(component, str) and isinstance(target, dict):
                    target = target.get(component)
                else: target = None
                if target is None: raise ValueError(f"Registered scenario path for {ident} no longer resolves.")
            if not isinstance(target, dict) or not isinstance(path[-1], str): raise ValueError(f"Registered scenario path for {ident} cannot be assigned.")
            target[path[-1]] = value
        elif adapter.startswith("closure_"):
            closure = closure or next((item for item in result.setdefault("disruptions", [])
                                       if item.get("type") == "route_closure" and item.get("resource_id") == definition["route_id"]), None)
            if closure is None:
                closure = {"disruption_id": "GOAL32-ROUTE-CLOSURE", "type": "route_closure",
                           "resource_id": definition["route_id"], "enabled": True}
                result["disruptions"].append(closure)
            closure["enabled"] = True
            if adapter == "closure_start":
                prior_start_text = closure.get("start_time")
                duration = 30.0
                current_end = closure.get("end_time")
                try:
                    prior_start = datetime.fromisoformat(str(prior_start_text or value).replace("Z", "+00:00"))
                    prior_end = datetime.fromisoformat(str(current_end).replace("Z", "+00:00")) if current_end else prior_start + timedelta(minutes=duration)
                    delta = prior_end - prior_start
                    if delta.total_seconds() <= 0: delta = timedelta(minutes=30)
                except (TypeError, ValueError):
                    delta = timedelta(minutes=30)
                closure["start_time"] = value
                end = datetime.fromisoformat(value.replace("Z", "+00:00")) + delta
                closure["end_time"] = end.isoformat().replace("+00:00", "Z")
            else:
                start = closure.get("start_time")
                if not start:
                    raise ValueError("Set a closure start parameter when sweeping closure duration.")
                end = datetime.fromisoformat(str(start).replace("Z", "+00:00")) + timedelta(minutes=float(value))
                closure["end_time"] = end.isoformat().replace("+00:00", "Z")
    return result


def _configurations(definition: dict[str, Any], studio: Any) -> tuple[list[dict[str, Any]], list[int], list[dict[str, Any]]]:
    base = definition.get("base_scenario")
    if not isinstance(base, dict):
        raise ValueError("An experiment needs a base scenario.")
    available = {item["id"]: item for item in registry(base, studio)}
    axes = definition.get("parameters", [])
    if not isinstance(axes, list) or not axes:
        raise ValueError("Choose at least one registered parameter.")
    seen: set[str] = set()
    expanded = []
    for axis in axes:
        ident = str(axis.get("id", ""))
        if ident not in available:
            raise ValueError(f"Unsupported parameter: {ident or '(missing id)'}")
        if ident in seen:
            raise ValueError(f"Duplicate parameter: {ident}")
        seen.add(ident)
        values = _axis_values(axis)
        for value in values: _validate_value(available[ident], value)
        expanded.append((ident, values))
    seeds = definition.get("seeds", [])
    if not isinstance(seeds, list) or not seeds or any(isinstance(seed, bool) or not isinstance(seed, int) or not 0 <= seed <= 2147483647 for seed in seeds):
        raise ValueError("Seeds must be a non-empty list of integers from 0 through 2,147,483,647.")
    if len(set(seeds)) != len(seeds):
        raise ValueError("Seed values must be unique.")
    analyzer_metrics = _load_analyzer(studio).METRICS
    constraints = definition.get("constraints", [])
    if not isinstance(constraints, list): raise ValueError("Constraints must be a list.")
    for constraint in constraints:
        if not isinstance(constraint, dict) or constraint.get("metric") not in analyzer_metrics:
            raise ValueError("Each constraint must use a supported Goal 19 numeric metric.")
        target = constraint.get("value")
        if constraint.get("op") not in {"eq", "le", "ge"} or isinstance(target, bool) or not isinstance(target, (int, float)) or not math.isfinite(target):
            raise ValueError("Each constraint needs eq, le, or ge and a finite numeric threshold.")
    ranking = definition.get("ranking", {"objectives": []})
    objectives = ranking.get("objectives", []) if isinstance(ranking, dict) else None
    if not isinstance(objectives, list): raise ValueError("Ranking objectives must be a list.")
    for objective in objectives:
        if not isinstance(objective, dict) or objective.get("metric") not in analyzer_metrics or objective.get("direction") not in {"minimize", "maximize"}:
            raise ValueError("Each ranking objective needs a supported Goal 19 metric and minimize or maximize direction.")
    run_count = (math.prod(len(values) for _ident, values in expanded) + (1 if definition.get("include_baseline") else 0)) * len(seeds)
    if run_count > MAX_RUNS and not definition.get("allow_large", False):
        raise ValueError(f"This experiment expands to {run_count} runs; the default limit is {MAX_RUNS}. Set allow_large to override.")
    configs = [{}]
    for ident, values in expanded:
        configs = [dict(current, **{ident: value}) for current in configs for value in values]
    rows = []
    for values in configs:
        config_id = "config_" + digest({"base_scenario": base, "parameters": values})[:12]
        rows.append({"configuration_id": config_id, "parameters": values, "scenario": _apply_parameters(base, values, available)})
    if definition.get("include_baseline"):
        rows.insert(0, {"configuration_id": "baseline_" + digest(base)[:12], "parameters": {"baseline": True}, "scenario": deepcopy(base)})
    return rows, list(seeds), [available[x[0]] for x in expanded]


def _load_analyzer(studio: Any) -> Any:
    path = studio.ROOT / "tools" / "experiment_analysis" / "analyze.py"
    spec = importlib.util.spec_from_file_location("ramplab_goal32_analyzer", path)
    if spec is None or spec.loader is None: raise RuntimeError("Goal 19 analysis could not be loaded.")
    module = importlib.util.module_from_spec(spec); sys.modules[spec.name] = module; spec.loader.exec_module(module)
    return module


def _metric_records(runs: list[dict[str, Any]], analyzer: Any) -> dict[str, Any]:
    by_config: dict[str, list[dict[str, Any]]] = {}
    for run in runs:
        by_config.setdefault(run["configuration_id"], []).append(run)
    configs = []
    for ident, rows in sorted(by_config.items()):
        metric_values: dict[str, list[float]] = {}
        for row in rows:
            for key, value in row.get("metrics", {}).items():
                if key in analyzer.METRICS and isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value):
                    metric_values.setdefault(key, []).append(float(value))
        aggregate = {}
        for key, values in sorted(metric_values.items()):
            aggregate[key] = {"count": len(values), "mean": sum(values) / len(values), "median": __import__("statistics").median(values),
                              "min": min(values), "max": max(values),
                              "standard_deviation": __import__("statistics").pstdev(values) if len(values) > 1 else None,
                              "unit": analyzer.METRICS[key].unit}
        configs.append({"configuration_id": ident, "parameters": rows[0]["parameters"], "status": _feasibility(rows, aggregate, rows[0].get("constraints", [])),
                        "metrics": aggregate, "run_count": len(rows), "successful_runs": sum(r["status"] in {"COMPLETED", "CACHED"} for r in rows),
                        "seeds": [r["seed"] for r in sorted(rows, key=lambda x: x["seed"])]})
    return {"configurations": configs}


def _feasibility(rows: list[dict[str, Any]], metrics: dict[str, Any], constraints: list[dict[str, Any]]) -> str:
    if any(row["status"] not in {"COMPLETED", "CACHED"} for row in rows): return "INCOMPLETE"
    for constraint in constraints:
        key = constraint["metric"]
        observed = [row.get("metrics", {}).get(key) for row in rows]
        observed = [value for value in observed if isinstance(value, (int, float)) and not isinstance(value, bool)]
        if not observed: return "INSUFFICIENT DATA"
        op, target = constraint["op"], constraint["value"]
        checks = {"eq": lambda x: x == target, "le": lambda x: x <= target, "ge": lambda x: x >= target}
        if op not in checks or not all(checks[op](value) for value in observed): return "INFEASIBLE"
    return "FEASIBLE" if constraints else "UNCONSTRAINED"


def _rank(configurations: list[dict[str, Any]], objectives: list[dict[str, str]]) -> list[dict[str, Any]]:
    feasible = []
    for row in configurations:
        if row["status"] not in {"FEASIBLE", "UNCONSTRAINED"}: continue
        values = []
        for objective in objectives:
            metric = row["metrics"].get(objective["metric"])
            if not metric or objective.get("direction") not in {"minimize", "maximize"}:
                values = []; break
            values.append(metric["mean"] * (1 if objective["direction"] == "minimize" else -1))
        if values: feasible.append((tuple(values), row["configuration_id"], row))
    feasible.sort(key=lambda item: (item[0], item[1]))
    for index, (_values, _ident, row) in enumerate(feasible, 1): row["rank"] = index
    eligible = [row for row in configurations if row["status"] in {"FEASIBLE", "UNCONSTRAINED"} and objectives and
                all(isinstance(row.get("metrics", {}).get(o["metric"], {}).get("mean"), (int, float)) for o in objectives)]
    def dominates(left: dict[str, Any], right: dict[str, Any]) -> bool:
        no_worse, strictly_better = True, False
        for objective in objectives:
            a = left["metrics"][objective["metric"]]["mean"]
            b = right["metrics"][objective["metric"]]["mean"]
            if objective["direction"] == "minimize": no_worse &= a <= b; strictly_better |= a < b
            else: no_worse &= a >= b; strictly_better |= a > b
        return no_worse and strictly_better
    for row in configurations:
        row["pareto"] = row in eligible and not any(other is not row and dominates(other, row) for other in eligible)
    return configurations


class ExperimentManager:
    def __init__(self, home: Path, studio: Any):
        self.home = Path(home)
        self.root = self.home / "experiments"
        self.root.mkdir(parents=True, exist_ok=True)
        self.cache_path = self.root / "run-cache.json"
        self.studio = studio
        self._lock = threading.RLock()
        self._cache_lock = threading.RLock()
        self._build_lock = threading.Lock()
        self._fingerprint_lock = threading.Lock()
        self._software_identity_cache: dict[str, Any] | None = None
        self._controls: dict[str, dict[str, Any]] = {}
        self._futures: dict[str, threading.Thread] = {}

    def _path(self, ident: str) -> Path:
        if len(ident) != 20 or any(char not in "0123456789abcdef" for char in ident): raise ValueError("Experiment identifier is invalid.")
        return self.root / ident

    def _read(self, ident: str) -> dict[str, Any]:
        path = self._path(ident) / "experiment.json"
        data = json.loads(path.read_text(encoding="utf-8"))
        if data.get("schema_version") != SCHEMA_VERSION: raise ValueError("Experiment schema version is unsupported.")
        return data

    def list(self) -> list[dict[str, Any]]:
        rows = []
        for path in sorted(self.root.glob("*/experiment.json")):
            try:
                data = json.loads(path.read_text(encoding="utf-8"))
                rows.append({k: data.get(k) for k in ("id", "name", "status", "created_at", "counts")})
            except (OSError, json.JSONDecodeError): continue
        return sorted(rows, key=lambda x: (x.get("created_at") or "", x.get("id") or ""), reverse=True)

    def get(self, ident: str) -> dict[str, Any]:
        return self._read(ident)

    def create(self, definition: dict[str, Any]) -> dict[str, Any]:
        configs, seeds, defs = _configurations(definition, self.studio)
        body = deepcopy(definition)
        body.pop("allow_large", None)
        ident = digest(body)[:20]
        folder = self._path(ident); folder.mkdir(parents=True, exist_ok=True)
        existing = folder / "experiment.json"
        if existing.is_file(): return self.get(ident)
        workers = definition.get("execution", {}).get("workers", "auto")
        if workers == "auto": workers = max(1, min(MAX_WORKERS, (os.cpu_count() or 2) - 1))
        if isinstance(workers, bool) or not isinstance(workers, int) or workers < 1: raise ValueError("Worker count must be a positive integer or auto.")
        workers = min(workers, MAX_WORKERS)
        runs = []
        for config in configs:
            for seed in seeds:
                runs.append({"run_id": "run_" + digest({"configuration_id": config["configuration_id"], "seed": seed})[:16],
                             "configuration_id": config["configuration_id"], "parameters": config["parameters"],
                             "scenario": config["scenario"], "seed": seed, "status": "QUEUED", "exit_status": None,
                             "started_at": None, "finished_at": None, "output_path": None,
                             "validation_status": "PENDING", "result_digest": None, "metrics": {}, "message": ""})
        now = datetime.now().astimezone().isoformat(timespec="seconds")
        result = {"schema_version": SCHEMA_VERSION, "id": ident, "name": str(body.get("name") or "Untitled experiment"),
                  "description": str(body.get("description", "")), "created_at": now, "updated_at": now,
                  "base_dataset": body.get("base_dataset", ""), "base_scenario": body.get("base_scenario"),
                  "parameters": [{**deepcopy(d), "values": _axis_values(next(a for a in body["parameters"] if a["id"] == d["id"]))} for d in defs],
                  "seeds": seeds, "execution": {"workers": workers, "worker_limit": MAX_WORKERS,
                                                   "cache_enabled": body.get("execution", {}).get("cache_enabled", True)},
                  "metrics_of_interest": body.get("metrics_of_interest", []), "constraints": body.get("constraints", []),
                  "ranking": body.get("ranking", {"objectives": []}), "provenance": {"generator_schema": "Goal24B", "analyzer": "Goal19",
                  "software_revision": subprocess.run(["git", "rev-parse", "HEAD"], cwd=self.studio.ROOT, capture_output=True, text=True).stdout.strip() or "unknown",
                  "platform": platform.platform()}, "configurations": [{k: v for k, v in item.items() if k != "scenario"} for item in configs],
                  "runs": runs, "counts": {"configurations": len(configs), "seeds": len(seeds), "total": len(runs), "completed": 0,
                                                "failed": 0, "invalid": 0, "cached": 0, "queued": len(runs), "running": 0},
                  "status": "DRAFT", "aggregate": {"configurations": []}, "ranking_result": [], "selected_candidate": None}
        self._write(result)
        # Store generated scenario bodies with the definition for safe resume.
        result["configurations"] = configs
        self._write(result)
        return result

    def _write(self, data: dict[str, Any]) -> None:
        data["updated_at"] = datetime.now().astimezone().isoformat(timespec="seconds")
        path = self._path(data["id"]) / "experiment.json"
        temp = path.with_name(f"experiment.{os.getpid()}.{threading.get_ident()}.tmp")
        temp.write_text(json.dumps(data, ensure_ascii=False, sort_keys=True, indent=2) + "\n", encoding="utf-8")
        for attempt in range(6):
            try:
                os.replace(temp, path)
                return
            except PermissionError:
                if attempt == 5: raise
                time.sleep(0.02 * (attempt + 1))

    @staticmethod
    def _refresh_counts(data: dict[str, Any]) -> None:
        counts = {status.lower(): sum(run["status"] == status for run in data["runs"])
                  for status in ("COMPLETED", "FAILED", "INVALID", "CACHED", "CANCELLED", "QUEUED", "RUNNING")}
        data["counts"] = {**data.get("counts", {}), **counts,
                          "completed": counts["completed"] + counts["cached"], "total": len(data["runs"])}

    def start(self, ident: str, workers: int | None = None) -> dict[str, Any]:
        with self._lock:
            data = self._read(ident)
            active = self._futures.get(ident)
            if data["status"] in RUNNING | {"PAUSED"} and active and active.is_alive(): return data
            if data["status"] in RUNNING | {"PAUSED"}:
                for run in data["runs"]:
                    if run["status"] == "RUNNING":
                        previous = Path(run.get("output_path") or "")
                        if previous.is_dir():
                            archived = previous.with_name(f"{previous.name}.interrupted-{time.time_ns()}")
                            previous.rename(archived)
                        run.update(status="QUEUED", validation_status="PENDING", output_path=None,
                                   message="Recovered an interrupted run; previous partial output was preserved.")
            if workers is not None:
                if isinstance(workers, bool) or not isinstance(workers, int) or workers < 1: raise ValueError("Worker count must be a positive integer.")
                data["execution"]["workers"] = min(workers, MAX_WORKERS)
            data["status"] = "RUNNING"; data["counts"]["queued"] = sum(run["status"] in {"QUEUED", "FAILED", "INVALID", "CANCELLED"} for run in data["runs"])
            self._refresh_counts(data)
            self._write(data)
            control = {"pause": threading.Event(), "stop": threading.Event()}
            self._controls[ident] = control
            thread = threading.Thread(target=self._execute, args=(ident,), name=f"ramplab-experiment-{ident}", daemon=True)
            self._futures[ident] = thread; thread.start()
            return data

    def control(self, ident: str, action: str) -> dict[str, Any]:
        control = self._controls.get(ident)
        if not control:
            if action == "resume": return self.start(ident)
            return self._read(ident)
        if action == "pause": control["pause"].set()
        elif action == "resume": control["pause"].clear()
        elif action == "stop": control["stop"].set()
        else: raise ValueError("Action must be pause, resume, or stop.")
        return self._read(ident)

    def _execute(self, ident: str) -> None:
        control = self._controls[ident]
        initial = self._read(ident)
        folder = self._path(ident)
        scenarios = {item["configuration_id"]: item["scenario"] for item in initial["configurations"]}
        with self._lock:
            for run in initial["runs"]:
                if run["status"] in {"FAILED", "INVALID", "CANCELLED"}: run["status"] = "QUEUED"
            self._refresh_counts(initial)
        self._write(initial)
        workers = max(1, min(int(initial["execution"]["workers"]), MAX_WORKERS))
        try:
            self._ensure_cli()
        except Exception as exc:
            for run in initial["runs"]:
                if run["status"] == "QUEUED":
                    run.update(status="FAILED", validation_status="FAILED", message=f"Simulator build failed: {exc}")
                    run["finished_at"] = datetime.now().astimezone().isoformat(timespec="seconds")
            self._refresh_counts(initial)
            initial["status"] = "COMPLETED_WITH_ERRORS"
            self._write(initial)
            return
        pending = [run for run in initial["runs"] if run["status"] == "QUEUED"]
        futures = {}
        with ThreadPoolExecutor(max_workers=workers, thread_name_prefix="ramplab-exp-run") as pool:
            while pending or futures:
                while pending and len(futures) < workers and not control["stop"].is_set():
                    if control["pause"].is_set(): break
                    run = pending.pop(0)
                    run["status"] = "RUNNING"; run["started_at"] = datetime.now().astimezone().isoformat(timespec="seconds")
                    run["output_path"] = str(folder / "runs" / run["run_id"])
                    run["constraints"] = deepcopy(initial.get("constraints", []))
                    run["_cache_enabled"] = initial["execution"].get("cache_enabled", True)
                    run["_software_revision"] = initial["provenance"].get("software_revision", "unknown")
                    self._refresh_counts(initial)
                    self._write(initial)
                    futures[pool.submit(self._execute_run, run, scenarios[run["configuration_id"]], folder)] = run
                if control["stop"].is_set():
                    for run in pending: run["status"] = "CANCELLED"; run["message"] = "Stopped before launch by user."
                    pending.clear()
                if not futures:
                    if pending and control["pause"].is_set(): time.sleep(0.1); continue
                    break
                for future in as_completed(tuple(futures), timeout=None):
                    run = futures.pop(future)
                    try: outcome = future.result()
                    except Exception as exc: outcome = {"status": "FAILED", "exit_status": None, "validation_status": "FAILED", "message": str(exc)}
                    run.update(outcome); run["finished_at"] = datetime.now().astimezone().isoformat(timespec="seconds")
                    run.pop("_cache_enabled", None); run.pop("_software_revision", None)
                    self._refresh_counts(initial)
                    self._write(initial)
                    break
        initial["aggregate"] = _metric_records(initial["runs"], _load_analyzer(self.studio))
        objectives = initial.get("ranking", {}).get("objectives", [])
        initial["ranking_result"] = _rank(initial["aggregate"]["configurations"], objectives)
        ranked = [r for r in initial["ranking_result"] if "rank" in r]
        initial["selected_candidate"] = ranked[0]["configuration_id"] if ranked else None
        counts = {status.lower(): sum(run["status"] == status for run in initial["runs"]) for status in ("COMPLETED", "FAILED", "INVALID", "CACHED", "CANCELLED", "QUEUED", "RUNNING")}
        self._refresh_counts(initial)
        initial["status"] = "PAUSED" if control["pause"].is_set() and counts["queued"] else "STOPPED" if control["stop"].is_set() else "COMPLETED" if not (counts["failed"] or counts["invalid"] or counts["cancelled"]) else "COMPLETED_WITH_ERRORS"
        self._write(initial)

    def _execute_run(self, run: dict[str, Any], scenario: dict[str, Any], experiment_folder: Path) -> dict[str, Any]:
        output = Path(run["output_path"]); output.mkdir(parents=True, exist_ok=True)
        scenario = deepcopy(scenario); scenario["seed"] = run["seed"]
        package = self.studio.scenario_package(scenario)
        mapping_path = self.studio._mapping(scenario.get("airport_key", "kauo"))
        mapping = self.studio.load_mapping(mapping_path)[0]
        geometry_path = Path(mapping_path).parent / mapping["geometry_file"]
        software_identity = self._software_identity(run.get("_software_revision", "unknown"))
        identity = digest({"base_dataset": package.get("dataset"), "canonical_scenario": package, "seed": run["seed"],
                           "generator_schema": "Goal24B", "software": software_identity,
                           "mapping_sha256": hashlib.sha256(Path(mapping_path).read_bytes()).hexdigest(),
                           "geometry_sha256": hashlib.sha256(geometry_path.read_bytes()).hexdigest()})
        with self._cache_lock:
            cache = _read_json(self.cache_path, {})
            if run.get("_cache_enabled", True) and identity in cache and _cached_valid(cache[identity]):
                return {"status": "CACHED", "exit_status": 0, "validation_status": "VALID", "result_digest": cache[identity]["result_digest"],
                        "metrics": cache[identity]["metrics"], "output_path": cache[identity]["output_path"], "run_identity": identity,
                        "software_fingerprint": software_identity, "message": "Validated identical result reused."}
        try:
            cli = self._ensure_cli()
            checked = self.studio.validate_scenario(scenario)
            if not checked.get("valid"):
                return {"status": "INVALID", "exit_status": None, "validation_status": "INVALID", "run_identity": identity,
                        "message": "; ".join(item.get("message", "Scenario invalid") for item in checked.get("findings", []) if item.get("severity") == "error")}
            package_file = output / "canonical.json"
            package_file.write_text(json.dumps(package, ensure_ascii=False, sort_keys=True, indent=2) + "\n", encoding="utf-8")
            generated = output / "generated"
            from tools.airport_scenario_generation.generator import generate, validate_generated
            generated_result = generate(package_file, self.studio._mapping(scenario.get("airport_key", "kauo")), generated, seed=run["seed"])
            validation = validate_generated(generated)
            if not validation.get("valid"): return {"status": "INVALID", "exit_status": None, "validation_status": "INVALID", "run_identity": identity, "message": "Generated scenario validation failed."}
            from tools.release.ramplab import run_scenario
            result_dir = output / "result"
            metadata = run_scenario(cli, None, "disruption", run["seed"], result_dir, scenario_file=generated / "scenario.json", capture_output=True)
            experiment_json = result_dir / "experiment.json"
            if not experiment_json.is_file(): raise ValueError("Native RampLab output lacks experiment.json.")
            analyzer = _load_analyzer(self.studio)
            parsed = analyzer.load_run(experiment_json)
            if not parsed.metrics: raise ValueError("Goal 19 found no supported numeric metrics in the native run output.")
            metric_path = result_dir / "simulator-metrics.json"
            metrics = _read_json(metric_path, parsed.metrics)
            projection = {"experiment": _stable_result_records(_read_json(experiment_json, {})),
                          "metrics": _stable_result_records(metrics),
                          "events": _stable_result_records(_read_jsonl(result_dir / "events.jsonl")),
                          "scenario_sha256": generated_result["manifest"].get("output_hashes", {}).get("scenario.json")}
            result_digest = digest(projection)
            outcome = {"status": "COMPLETED", "exit_status": 0, "validation_status": "VALID", "run_identity": identity,
                       "result_digest": result_digest, "metrics": parsed.metrics, "goal19_identity": parsed.identity,
                       "software_fingerprint": software_identity,
                       "native_scenario_sha256": projection["scenario_sha256"], "run_metadata": metadata, "message": ""}
            with self._cache_lock:
                cache = _read_json(self.cache_path, {})
                cache[identity] = {"result_digest": result_digest, "metrics": parsed.metrics, "output_path": str(result_dir), "validation_status": "VALID",
                                   "artifact_hashes": {name: hashlib.sha256((result_dir / name).read_bytes()).hexdigest()
                                                       for name in ("experiment.json", "runs.csv", "simulator-metrics.json", "events.jsonl") if (result_dir / name).is_file()}}
                if run.get("_cache_enabled", True):
                    self.cache_path.write_text(json.dumps(cache, ensure_ascii=False, sort_keys=True, indent=2) + "\n", encoding="utf-8")
            return outcome
        except subprocess.CalledProcessError as exc:
            return {"status": "FAILED", "exit_status": exc.returncode, "validation_status": "NOT_REACHED", "run_identity": identity,
                    "message": (exc.stderr or exc.stdout or str(exc))[-1200:]}
        except (OSError, ValueError, TypeError, KeyError, RuntimeError, subprocess.TimeoutExpired) as exc:
            return {"status": "FAILED", "exit_status": None, "validation_status": "FAILED", "run_identity": identity, "message": str(exc)[:1200]}

    def _ensure_cli(self) -> Path:
        build = self.studio.ROOT / "build-goal32"
        cli = self.studio._resolve_cli(build)
        if cli.is_file(): return cli
        with self._build_lock:
            cli = self.studio._resolve_cli(build)
            if cli.is_file(): return cli
            cmake = __import__("shutil").which("cmake")
            if not cmake: raise RuntimeError("CMake is required to build the RampLab simulator.")
            subprocess.run([cmake, "-S", str(self.studio.ROOT), "-B", str(build)], cwd=self.studio.ROOT, check=True, capture_output=True, text=True, timeout=1200)
            subprocess.run([cmake, "--build", str(build), "--config", "Release", "--target", "airside_cli", "--parallel", "1"], cwd=self.studio.ROOT, check=True, capture_output=True, text=True, timeout=1800)
            return self.studio._resolve_cli(build)

    def _software_identity(self, revision: str) -> dict[str, Any]:
        if self._software_identity_cache is not None: return self._software_identity_cache
        with self._fingerprint_lock:
            if self._software_identity_cache is not None: return self._software_identity_cache
            cli = self.studio._resolve_cli(self.studio.ROOT / "build-goal32")
            sources = ("tools/ops_dashboard/experiments.py", "tools/ops_dashboard/scenario_studio.py",
                       "tools/airport_scenario_generation/generator.py", "tools/airport_scenario_generation/run_artifacts.py",
                       "tools/release/ramplab.py", "tools/experiment_analysis/analyze.py")
            self._software_identity_cache = {
                "git_revision": revision,
                "airside_cli_sha256": hashlib.sha256(cli.read_bytes()).hexdigest() if cli.is_file() else "unavailable",
                "pipeline_source_sha256": {name: hashlib.sha256((self.studio.ROOT / name).read_bytes()).hexdigest()
                                           for name in sources if (self.studio.ROOT / name).is_file()},
            }
            return self._software_identity_cache


def _read_json(path: Path, fallback: Any) -> Any:
    try: return json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError): return fallback


def _read_text(path: Path) -> str:
    try: return Path(path).read_text(encoding="utf-8")
    except OSError: return ""


def _read_jsonl(path: Path) -> list[Any]:
    try:
        return [json.loads(line) for line in Path(path).read_text(encoding="utf-8").splitlines() if line.strip()]
    except (OSError, json.JSONDecodeError): return []


def _stable_result_records(value: Any) -> Any:
    """Normalize observational wall-clock fields using Goal 19 determinism rules."""
    volatile = {"execution_ms", "execution_seconds", "completed_at_utc"}
    if isinstance(value, dict):
        return {key: _stable_result_records(item) for key, item in value.items() if key not in volatile}
    if isinstance(value, list): return [_stable_result_records(item) for item in value]
    return value


def _cached_valid(item: Any) -> bool:
    if not isinstance(item, dict) or item.get("validation_status") != "VALID" or not item.get("result_digest"): return False
    path = Path(str(item.get("output_path", "")))
    hashes = item.get("artifact_hashes")
    required = {"experiment.json", "runs.csv", "simulator-metrics.json", "events.jsonl"}
    if not isinstance(hashes, dict) or set(hashes) != required: return False
    if not path.is_dir(): return False
    try:
        return all((path / name).is_file() and hashlib.sha256((path / name).read_bytes()).hexdigest() == expected
                   for name, expected in hashes.items())
    except OSError:
        return False


def report_markdown(data: dict[str, Any]) -> str:
    counts = data.get("counts", {})
    configs = data.get("ranking_result", [])
    top = sorted((item for item in configs if item.get("rank")), key=lambda item: item["rank"])
    out = [f"# {data.get('name', 'RampLab experiment')}", "", str(data.get("description", "")), "",
           "## Experiment overview", "", f"- Base dataset: `{data.get('base_dataset', 'unavailable')}`",
           f"- Tested configurations: {counts.get('configurations', 0)}", f"- Seeds: {', '.join(str(seed) for seed in data.get('seeds', []))}",
           f"- Total simulation runs: {counts.get('total', 0)}", f"- Completed: {counts.get('completed', 0)}; failed: {counts.get('failed', 0)}; invalid: {counts.get('invalid', 0)}; cached: {counts.get('cached', 0)}",
           f"- Worker limit: {data.get('execution', {}).get('workers', 'unknown')}", "", "## Parameter space", "",
           "| Parameter | Values |", "|---|---|"]
    for axis in data.get("parameters", []): out.append(f"| `{axis.get('id')}` | {', '.join(map(str, axis.get('values', [])))} |")
    out += ["", "## Constraints and objectives", ""]
    out.extend(f"- `{item.get('metric')}` {item.get('op')} {item.get('value')}" for item in data.get("constraints", []))
    out.extend(f"- {item.get('direction')} `{item.get('metric')}`" for item in data.get("ranking", {}).get("objectives", []))
    if not data.get("constraints"): out.append("No hard constraints were configured.")
    out += ["", "## Highest-ranked tested configurations", "", "| Rank | Configuration | Status | Metrics |", "|---:|---|---|---|"]
    for item in top[:10]:
        vals = "; ".join(f"{key}={metric['mean']:.3f} {metric['unit']}" for key, metric in sorted(item.get("metrics", {}).items()))
        out.append(f"| {item['rank']} | `{item['configuration_id']}` | {item['status']} | {vals} |")
    baseline = next((item for item in configs if item["configuration_id"].startswith("baseline_")), None)
    candidate = next((item for item in configs if item["configuration_id"] == data.get("selected_candidate")), None)
    out += ["", "## Baseline comparison", ""]
    if baseline and candidate:
        if baseline["configuration_id"] == candidate["configuration_id"]:
            out.append("The highest-ranked tested configuration is the unchanged baseline; no tested variant outranked it.")
        out += ["| Metric | Baseline mean | Candidate mean | Candidate minus baseline |", "|---|---:|---:|---:|"]
        for key in sorted(set(baseline.get("metrics", {})) & set(candidate.get("metrics", {}))):
            left = baseline["metrics"][key]; right = candidate["metrics"][key]
            delta = right["mean"] - left["mean"]
            out.append(f"| `{key}` | {left['mean']:.3f} {left['unit']} | {right['mean']:.3f} {right['unit']} | {delta:+.3f} {right['unit']} |")
        if baseline["configuration_id"] != candidate["configuration_id"]:
            out.append(f"Candidate parameters: `{json.dumps(candidate.get('parameters', {}), sort_keys=True)}`")
    else:
        out.append("Baseline or selected candidate metrics are unavailable; no comparison is inferred.")
    objective = (data.get("ranking", {}).get("objectives") or [{}])[0].get("metric")
    out += ["", "## Observed parameter response", ""]
    sensitivity_rows = 0
    for axis in data.get("parameters", []):
        by_value: dict[str, list[float]] = {}
        for config in configs:
            value = config.get("parameters", {}).get(axis.get("id"))
            metric = config.get("metrics", {}).get(objective or "")
            if value is not None and metric and config.get("status") not in {"INCOMPLETE", "INSUFFICIENT DATA"}:
                by_value.setdefault(str(value), []).append(float(metric["mean"]))
        if not by_value: continue
        out += [f"### {axis.get('name', axis.get('id'))}", "", f"Response metric: `{objective}`", "",
                "| Tested value | Mean across configurations |", "|---|---:|"]
        for value in sorted(by_value):
            values = by_value[value]
            out.append(f"| {value} | {sum(values) / len(values):.3f} ({len(values)} configurations) |")
            sensitivity_rows += 1
    if not sensitivity_rows: out.append("No complete parameter groups have the selected metric available.")
    out.append("These are observed associations in the tested parameter space; they are not causal estimates.")
    out += ["", "## Pareto candidates", ""]
    frontier = [item for item in configs if item.get("pareto") and item.get("status") == "FEASIBLE"]
    out.extend(f"- `{item['configuration_id']}`" for item in frontier) if frontier else out.append("No feasible Pareto candidate has sufficient data.")
    out += ["", "## Reproducibility", "", f"- Schema version: {data.get('schema_version')}",
            f"- Experiment ID: `{data.get('id')}`", f"- Software revision: `{data.get('provenance', {}).get('software_revision', 'unavailable')}`",
            "- Individual run identities, result digests, validation states, and output paths are in `experiment.json`.",
            "- Sensitivity values are observed parameter associations; they do not establish causation.", ""]
    return "\n".join(out)
