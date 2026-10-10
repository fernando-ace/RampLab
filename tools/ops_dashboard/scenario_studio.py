"""No-code scenario editing and real RampLab generation/run orchestration."""
from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
from copy import deepcopy
from datetime import datetime
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import threading
import time
import traceback
import uuid
from io import BytesIO
from typing import Any
from zipfile import ZIP_DEFLATED, ZipFile

ROOT = Path(__file__).resolve().parents[2]
INGESTION = ROOT / "tools" / "airport_data_ingestion"
GENERATION = ROOT / "tools" / "airport_scenario_generation"
RELEASE = ROOT / "tools" / "release"
DEFAULT_HOME = Path(os.environ.get("LOCALAPPDATA", str(Path.home()))) / "RampLab" / "ScenarioStudio"
HOME = Path(os.environ.get("RAMPLAB_SCENARIO_STUDIO_HOME", str(DEFAULT_HOME))).expanduser().resolve()
HOME.mkdir(parents=True, exist_ok=True)
STORE = HOME / "saved-scenarios"
RUNS = HOME / "runs"
STORE.mkdir(parents=True, exist_ok=True)
RUNS.mkdir(parents=True, exist_ok=True)

sys.path.insert(0, str(ROOT))
from tools.airport_data_ingestion.core import load_dataset  # noqa: E402
from tools.airport_scenario_generation.generator import (  # noqa: E402
    GenerationError,
    generate,
    inspect_generated,
    load_mapping,
    load_package,
    validate_generated,
)


def _manifest(key: str) -> Path:
    choices = {
        "kauo": INGESTION / "examples" / "kauo" / "manifest.json",
        "kauo-goal27": INGESTION / "examples" / "kauo_goal27" / "manifest.json",
        "synthetic": INGESTION / "examples" / "synthetic" / "manifest.json",
    }
    try:
        return choices[key]
    except KeyError as exc:
        raise ValueError("Choose one of the available RampLab airport datasets.") from exc


def _mapping(key: str) -> Path:
    choices = {
        "kauo": GENERATION / "mapping.kauo.json",
        "kauo-goal27": GENERATION / "mapping.kauo_goal27.json",
        "synthetic": GENERATION / "mapping.synthetic.json",
    }
    return choices[key]


def load_base(key: str = "kauo") -> tuple[dict[str, Any], dict[str, Any], dict[str, Any]]:
    manifest = _manifest(key)
    data, findings, _counts = load_dataset(manifest)
    if data is None or data.get("validation", {}).get("status") != "valid":
        details = "; ".join(item.get("message", "Invalid source data") for item in findings[:4])
        raise ValueError(f"The selected airport dataset did not pass Goal 24A validation. {details}")
    mapping, _mapping_raw, geometry, _geometry_raw = load_mapping(_mapping(key))
    return data, mapping, geometry


def _now_stamp() -> str:
    return datetime.now().astimezone().isoformat(timespec="minutes")


def new_scenario(key: str = "kauo") -> dict[str, Any]:
    data, _mapping, _geometry = load_base(key)
    # The baseline is the same canonical dataset with its authored disruptions
    # turned off. A variant adds only explicit edits and never changes this base.
    baseline = deepcopy(data)
    baseline["disruptions"] = []
    return {
        "schema_version": 1,
        "id": "",
        "name": f"{data['airport']['airport_id']} baseline",
        "airport_key": key,
        "dataset_id": data["dataset"]["dataset_id"],
        "seed": 42,
        "baseline": True,
        "flights": deepcopy(baseline["flights"]),
        "aircraft": deepcopy(baseline["aircraft"]),
        "gates": deepcopy(baseline["gates"]),
        "equipment": deepcopy(baseline["equipment"]),
        "turnaround_requirements": deepcopy(baseline["turnaround_requirements"]),
        "disruptions": [],
        "created_at": _now_stamp(),
    }


def scenario_package(scenario: dict[str, Any]) -> dict[str, Any]:
    data, _mapping, _geometry = load_base(str(scenario.get("airport_key", "kauo")))
    for name in ("flights", "aircraft", "gates", "equipment", "turnaround_requirements", "disruptions"):
        value = scenario.get(name)
        if not isinstance(value, list):
            raise ValueError(f"{name.replace('_', ' ').title()} must be a list.")
        data[name] = deepcopy(value)
    data["disruptions"] = [row for row in data["disruptions"] if row.get("enabled") is not False]
    data["schema_version"] = "1.0"
    return data


def _timestamp(value: Any, label: str, errors: list[dict[str, str]]) -> datetime | None:
    if not isinstance(value, str) or not value.strip():
        errors.append({"severity": "error", "code": "TIME_REQUIRED", "message": f"{label} needs a date and time."})
        return None
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError:
        errors.append({"severity": "error", "code": "TIME_INVALID", "message": f"{label} is not a valid date and time."})
        return None
    if parsed.tzinfo is None:
        errors.append({"severity": "error", "code": "TIME_ZONE", "message": f"{label} needs a UTC offset."})
        return None
    return parsed


def validate_scenario(scenario: dict[str, Any]) -> dict[str, Any]:
    findings: list[dict[str, str]] = []
    try:
        data = scenario_package(scenario)
        mapping, _raw, geometry, _geometry_raw = load_mapping(_mapping(str(scenario.get("airport_key", "kauo"))))
    except (ValueError, KeyError, TypeError, GenerationError) as exc:
        return {"valid": False, "findings": [{"severity": "error", "code": "DATASET_INVALID", "message": str(exc)}]}

    seed = scenario.get("seed")
    if not isinstance(seed, int) or isinstance(seed, bool) or seed < 0 or seed > 2147483647:
        findings.append({"severity": "error", "code": "SEED_INVALID", "message": "Seed must be a whole number from 0 through 2,147,483,647."})

    for section, field, label in (("flights", "flight_id", "Flight identifier"),
                                  ("aircraft", "aircraft_id", "Aircraft identifier"),
                                  ("gates", "gate_id", "Stand identifier"),
                                  ("equipment", "equipment_id", "Vehicle identifier"),
                                  ("turnaround_requirements", "requirement_id", "Service identifier"),
                                  ("disruptions", "disruption_id", "Disruption identifier")):
        seen: set[str] = set()
        for row in data[section]:
            ident = str(row.get(field, "")).strip()
            if not ident:
                findings.append({"severity": "error", "code": "ID_REQUIRED", "message": f"{label} cannot be blank."})
            elif ident in seen:
                findings.append({"severity": "error", "code": "ID_DUPLICATE", "message": f"{label} {ident} appears more than once."})
            seen.add(ident)

    aircraft_ids = {str(row.get("aircraft_id")) for row in data["aircraft"]}
    gate_ids = {str(row.get("gate_id")) for row in data["gates"]}
    flight_ids = {str(row.get("flight_id")) for row in data["flights"]}
    equipment_ids = {str(row.get("equipment_id")) for row in data["equipment"]}
    requirement_ids = {str(row.get("requirement_id")) for row in data["turnaround_requirements"]}
    mapped_routes = set(mapping.get("route_ids", {}))
    mapped_edges = {str(edge.get("id")) for edge in geometry.get("airport", {}).get("edges", [])}
    services = set(mapping.get("service_types", {}))
    aircraft_schedule: dict[str, dict[str, datetime]] = {}
    for flight in data["flights"]:
        if str(flight.get("aircraft_id")) not in aircraft_ids:
            findings.append({"severity": "error", "code": "AIRCRAFT_UNKNOWN", "message": f"{flight.get('flight_id', 'Flight')} refers to an aircraft that is not in this scenario."})
        if str(flight.get("gate_id")) not in gate_ids:
            findings.append({"severity": "error", "code": "STAND_UNKNOWN", "message": f"{flight.get('flight_id', 'Flight')} needs an available stand."})
        if flight.get("operation") not in {"arrival", "departure"}:
            findings.append({"severity": "error", "code": "OPERATION_INVALID", "message": f"{flight.get('flight_id', 'Flight')} must be an arrival or departure."})
        when = _timestamp(flight.get("scheduled_time"), f"{flight.get('flight_id', 'Flight')} time", findings)
        if when:
            operation_times = aircraft_schedule.setdefault(str(flight.get("aircraft_id")), {})
            operation = str(flight.get("operation"))
            if operation in operation_times:
                findings.append({"severity": "error", "code": "AIRCRAFT_OPERATION_DUPLICATE", "message": f"An aircraft can have at most one arrival and one departure in this scenario ({flight.get('aircraft_id')})."})
            operation_times[operation] = when
    for aircraft_id, operations in aircraft_schedule.items():
        if "arrival" in operations and "departure" in operations and operations["departure"] < operations["arrival"]:
            findings.append({"severity": "error", "code": "DEPARTURE_BEFORE_ARRIVAL", "message": f"{aircraft_id}'s departure is scheduled before its arrival."})
    for req in data["turnaround_requirements"]:
        if req.get("aircraft_id") and str(req["aircraft_id"]) not in aircraft_ids:
            findings.append({"severity": "error", "code": "SERVICE_AIRCRAFT_UNKNOWN", "message": f"Service {req.get('requirement_id')} refers to an unknown aircraft."})
        if req.get("service_type") not in services:
            findings.append({"severity": "error", "code": "SERVICE_UNSUPPORTED", "message": f"{req.get('service_type')} is not supported by the selected scenario generator."})
        if not isinstance(req.get("duration_minutes"), (int, float)) or isinstance(req.get("duration_minutes"), bool) or req["duration_minutes"] <= 0:
            findings.append({"severity": "error", "code": "SERVICE_DURATION_INVALID", "message": f"Service {req.get('requirement_id')} must have a duration greater than zero."})
    for item in data["equipment"]:
        if item.get("type") not in mapping.get("equipment_types", {}):
            findings.append({"severity": "warning", "code": "EQUIPMENT_UNSUPPORTED", "message": f"{item.get('type')} has no simulator vehicle mapping and remains read-only metadata."})
        node = str(item.get("initial_location", ""))
        node_ids = {str(n.get("id")) for n in geometry.get("airport", {}).get("nodes", [])}
        if item.get("type") in mapping.get("equipment_types", {}) and item.get("status") in {"available", "active", "open"} and node and node not in node_ids:
            findings.append({"severity": "error", "code": "VEHICLE_LOCATION_UNKNOWN", "message": f"{item.get('equipment_id')} is assigned to an unknown airport location."})
    for disruption in data["disruptions"]:
        kind = disruption.get("type")
        start = _timestamp(disruption.get("start_time"), f"{disruption.get('disruption_id', 'Disruption')} start", findings)
        end = _timestamp(disruption.get("end_time"), f"{disruption.get('disruption_id', 'Disruption')} end", findings) if disruption.get("end_time") else None
        if start and end and end <= start:
            findings.append({"severity": "error", "code": "INTERVAL_INVALID", "message": "A disruption must end after it starts."})
        if kind == "route_closure":
            if disruption.get("resource_id") not in mapped_routes:
                findings.append({"severity": "error", "code": "ROUTE_UNSUPPORTED", "message": "This route has no supported closure mapping for the selected airport."})
            elif mapping["route_ids"][disruption["resource_id"]] not in mapped_edges:
                findings.append({"severity": "error", "code": "ROUTE_UNKNOWN", "message": "The selected route no longer matches the airport map."})
        elif kind == "equipment_outage":
            target = str(disruption.get("equipment_id", ""))
            if target not in equipment_ids:
                findings.append({"severity": "error", "code": "VEHICLE_UNKNOWN", "message": "Choose a vehicle in this scenario for the outage."})
            if disruption.get("end_time") or disruption.get("duration_minutes"):
                findings.append({"severity": "error", "code": "OUTAGE_DURATION_UNSUPPORTED", "message": "Vehicle outages currently remove availability for the rest of the run; a timed recovery is not supported."})
        elif kind == "flight_delay":
            if str(disruption.get("flight_id", "")) not in flight_ids:
                findings.append({"severity": "error", "code": "FLIGHT_UNKNOWN", "message": "Choose a flight in this scenario for the delay."})
            delay = disruption.get("delay_minutes", disruption.get("duration_minutes", 0))
            if not isinstance(delay, (int, float)) or isinstance(delay, bool) or delay <= 0:
                findings.append({"severity": "error", "code": "DELAY_INVALID", "message": "A flight delay must be greater than zero minutes."})
        elif kind == "delayed_service":
            req_id = str(disruption.get("requirement_id", ""))
            flight_id = str(disruption.get("flight_id", ""))
            if req_id not in requirement_ids and flight_id not in flight_ids:
                findings.append({"severity": "error", "code": "SERVICE_TARGET_UNKNOWN", "message": "Choose a service or flight in this scenario for the delay."})
            delay = disruption.get("delay_minutes", disruption.get("duration_minutes", 0))
            if not isinstance(delay, (int, float)) or isinstance(delay, bool) or delay <= 0:
                findings.append({"severity": "error", "code": "DELAY_INVALID", "message": "A service delay must be greater than zero minutes."})
        else:
            findings.append({"severity": "error", "code": "DISRUPTION_UNSUPPORTED", "message": "This disruption type is not supported by the selected RampLab generator."})

    # Exercise Goal 24B itself so validation catches mapping, resource, and
    # generated-reference failures before the run button can launch the engine.
    if not any(item["severity"] == "error" for item in findings):
        with __import__("tempfile").TemporaryDirectory(prefix="ramplab-studio-validate-") as temp:
            root = Path(temp)
            package_file = root / "canonical.json"
            output_dir = root / "generated"
            try:
                package_file.write_text(json.dumps(data, ensure_ascii=False, sort_keys=True, indent=2) + "\n", encoding="utf-8")
                generate(package_file, _mapping(str(scenario.get("airport_key", "kauo"))), output_dir,
                         seed=int(scenario.get("seed", 42)))
                validate_generated(output_dir)
            except (GenerationError, OSError, ValueError, TypeError, KeyError) as exc:
                findings.append({"severity": "error", "code": "GENERATOR_VALIDATION", "message": f"RampLab cannot generate this scenario: {exc}"})
    active_types = {str(row.get("type")) for row in data["equipment"] if row.get("status") in {"available", "active", "open"}}
    if not active_types:
        findings.append({"severity": "info", "code": "NO_ACTIVE_VEHICLES", "message": "This dataset has no mapped mobile vehicles. Fleet outage edits are unavailable."})
    if not data["disruptions"]:
        findings.append({"severity": "info", "code": "BASELINE_ONLY", "message": "No disruptions are active. This is a baseline run."})
    errors = sum(item["severity"] == "error" for item in findings)
    return {"valid": errors == 0, "status": "ready" if errors == 0 else "blocked", "findings": findings,
            "summary": {"flights": len(data["flights"]), "aircraft": len(data["aircraft"]), "vehicles": len(data["equipment"]),
                        "stands": len(data["gates"]), "disruptions": len(data["disruptions"])}}


def scenario_diff(baseline: dict[str, Any], variant: dict[str, Any]) -> list[dict[str, Any]]:
    changes: list[dict[str, Any]] = []
    base_flights = {str(row.get("flight_id")): row for row in baseline.get("flights", [])}
    for flight in variant.get("flights", []):
        ident = str(flight.get("flight_id"))
        before = base_flights.get(ident)
        if before is None:
            changes.append({"area": "Flights", "label": f"Added flight {ident}"})
            continue
        for key, label in (("scheduled_time", "scheduled time"), ("gate_id", "stand"), ("aircraft_id", "aircraft"), ("operation", "operation")):
            if before.get(key) != flight.get(key):
                changes.append({"area": "Flights", "label": f"{ident} {label}", "before": before.get(key), "after": flight.get(key)})
    for ident in base_flights.keys() - {str(row.get("flight_id")) for row in variant.get("flights", [])}:
        changes.append({"area": "Flights", "label": f"Removed flight {ident}"})
    base_equipment = {str(row.get("equipment_id")): row for row in baseline.get("equipment", [])}
    for item in variant.get("equipment", []):
        ident = str(item.get("equipment_id"))
        before = base_equipment.get(ident)
        if before is None:
            changes.append({"area": "Fleet", "label": f"Added vehicle {ident}"})
            continue
        for key, label in (("status", "availability"), ("initial_location", "initial location"), ("type", "equipment type")):
            if before.get(key) != item.get(key):
                changes.append({"area": "Fleet", "label": f"{ident} {label}", "before": before.get(key), "after": item.get(key)})
    for req in variant.get("turnaround_requirements", []):
        before = next((row for row in baseline.get("turnaround_requirements", []) if row.get("requirement_id") == req.get("requirement_id")), None)
        if before and before.get("duration_minutes") != req.get("duration_minutes"):
            changes.append({"area": "Turnarounds", "label": f"{req.get('requirement_id')} service duration", "before": before.get("duration_minutes"), "after": req.get("duration_minutes")})
    for disruption in variant.get("disruptions", []):
        if disruption.get("type") == "route_closure":
            changes.append({"area": "Disruptions", "label": "Taxiway closure", "target": disruption.get("resource_id"),
                            "start": disruption.get("start_time"), "end": disruption.get("end_time")})
        else:
            changes.append({"area": "Disruptions", "label": str(disruption.get("type", "Disruption")).replace("_", " ").title(),
                            "target": disruption.get("equipment_id", disruption.get("flight_id", disruption.get("requirement_id")))})
    if int(variant.get("seed", 42)) != int(baseline.get("seed", 42)):
        changes.append({"area": "Environment", "label": "Random seed", "before": baseline.get("seed", 42), "after": variant.get("seed", 42)})
    return changes


def _read_json(path: Path, fallback: Any) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return fallback


class ScenarioStore:
    def list(self) -> list[dict[str, Any]]:
        summaries = []
        for path in sorted(STORE.glob("*.json"), key=lambda item: item.stat().st_mtime, reverse=True):
            value = _read_json(path, None)
            if isinstance(value, dict):
                summaries.append({"id": value.get("id"), "name": value.get("name"), "airport_key": value.get("airport_key"),
                                  "saved_at": value.get("saved_at"), "disruptions": len(value.get("disruptions", []))})
        return summaries

    def save(self, scenario: dict[str, Any]) -> dict[str, Any]:
        if not isinstance(scenario, dict) or scenario.get("schema_version") != 1:
            raise ValueError("This saved scenario version is not supported.")
        validate = validate_scenario(scenario)
        if not validate["valid"]:
            raise ValueError("Fix the scenario errors before saving.")
        saved = deepcopy(scenario)
        ident = str(saved.get("id") or uuid.uuid4().hex)
        if not re.fullmatch(r"[a-f0-9]{32}", ident):
            raise ValueError("The scenario identifier is invalid.")
        saved["id"] = ident
        saved["saved_at"] = _now_stamp()
        (STORE / f"{ident}.json").write_text(json.dumps(saved, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        return saved

    def get(self, ident: str) -> dict[str, Any]:
        if not re.fullmatch(r"[a-f0-9]{32}", ident):
            raise ValueError("The saved scenario identifier is invalid.")
        data = _read_json(STORE / f"{ident}.json", None)
        if not isinstance(data, dict):
            raise FileNotFoundError("That saved scenario could not be found.")
        return data


class RunManager:
    def __init__(self) -> None:
        self._lock = threading.RLock()
        self._executor = ThreadPoolExecutor(max_workers=2, thread_name_prefix="ramplab-studio")
        self._records: dict[str, dict[str, Any]] = {}
        for path in RUNS.glob("*.json"):
            record = _read_json(path, None)
            if isinstance(record, dict) and record.get("id"):
                if record.get("status") in {"queued", "preparing", "generating", "running"}:
                    record["status"] = "failed"
                    record["message"] = "The server restarted before this run completed. Start a new run to continue."
                    self._persist(record)
                self._records[record["id"]] = record

    def _persist(self, record: dict[str, Any]) -> None:
        (RUNS / f"{record['id']}.json").write_text(json.dumps(record, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    def list(self) -> list[dict[str, Any]]:
        with self._lock:
            return sorted((deepcopy(item) for item in self._records.values()), key=lambda item: item.get("created_at", ""), reverse=True)

    def get(self, ident: str) -> dict[str, Any]:
        if not re.fullmatch(r"[a-f0-9]{32}", ident):
            raise ValueError("The run identifier is invalid.")
        with self._lock:
            if ident not in self._records:
                raise FileNotFoundError("That run could not be found.")
            return deepcopy(self._records[ident])

    def _update(self, ident: str, **updates: Any) -> None:
        with self._lock:
            record = self._records[ident]
            record.update(updates)
            record["updated_at"] = _now_stamp()
            self._persist(record)

    def launch(self, scenario: dict[str, Any]) -> dict[str, Any]:
        checked = validate_scenario(scenario)
        if not checked["valid"]:
            raise ValueError("Resolve the validation errors before running this scenario.")
        fingerprint = json.dumps({key: scenario.get(key) for key in ("airport_key", "seed", "flights", "aircraft", "gates", "equipment", "turnaround_requirements", "disruptions")}, sort_keys=True)
        with self._lock:
            for record in self._records.values():
                if record.get("fingerprint") == fingerprint and record.get("status") in {"queued", "preparing", "generating", "running"}:
                    raise ValueError("This same scenario is already running. Wait for it to finish before starting another copy.")
            ident = uuid.uuid4().hex
            folder = RUNS / ident
            folder.mkdir(parents=True)
            record = {"id": ident, "scenario_name": str(scenario.get("name") or "Untitled scenario"),
                      "airport_key": scenario.get("airport_key", "kauo"), "seed": int(scenario.get("seed", 42)),
                      "status": "queued", "progress": "Waiting for a simulator slot", "message": "",
                      "created_at": _now_stamp(), "updated_at": _now_stamp(), "fingerprint": fingerprint,
                      "scenario": deepcopy(scenario), "folder": str(folder), "result": None}
            self._records[ident] = record
            self._persist(record)
            self._executor.submit(self._execute, ident, scenario)
            return deepcopy(record)

    def _execute(self, ident: str, scenario: dict[str, Any]) -> None:
        folder = Path(self.get(ident)["folder"])
        try:
            self._update(ident, status="preparing", progress="Checking the RampLab simulator", message="")
            cli = _resolve_cli()
            if not cli.is_file():
                self._update(ident, progress="Building the RampLab simulator")
                cmake = shutil.which("cmake")
                if not cmake:
                    raise RuntimeError("CMake is required to build the RampLab simulator. Install CMake and reopen the Studio.")
                build = ROOT / "build-goal30"
                subprocess.run([cmake, "-S", str(ROOT), "-B", str(build)], cwd=ROOT, check=True, capture_output=True, text=True, timeout=1200)
                subprocess.run([cmake, "--build", str(build), "--config", "Release", "--target", "airside_cli", "--parallel"],
                               cwd=ROOT, check=True, capture_output=True, text=True, timeout=1800)
                cli = _resolve_cli(build)
                if not cli.is_file():
                    raise RuntimeError("The simulator build finished without producing its command-line runner.")
            self._update(ident, status="generating", progress="Generating and validating the RampLab scenario")
            data = scenario_package(scenario)
            package_file = folder / "canonical.json"
            package_file.write_text(json.dumps(data, ensure_ascii=False, sort_keys=True, indent=2) + "\n", encoding="utf-8")
            generated = folder / "generated"
            generated_result = generate(package_file, _mapping(scenario.get("airport_key", "kauo")), generated,
                                        seed=int(scenario.get("seed", 42)))
            validation = validate_generated(generated)
            self._update(ident, status="running", progress="Running the native RampLab simulation")
            from tools.release.ramplab import run_scenario
            output = folder / "result"
            start = time.monotonic()
            metadata = run_scenario(cli, None, "disruption", int(scenario.get("seed", 42)), output,
                                    scenario_file=generated / "scenario.json", capture_output=True)
            wall = round(time.monotonic() - start, 3)
            metrics = _read_json(output / "simulator-metrics.json", {})
            result = {"scenario": metadata, "generation": {"scenario_sha256": generated_result["manifest"].get("output_hashes", {}).get("scenario.json"),
                                                              "validation": validation},
                      "metrics": metrics, "wall_seconds": wall,
                      "artifact_names": sorted(path.name for path in output.iterdir() if path.is_file()),
                      "folder": str(folder), "status": "complete"}
            self._update(ident, status="complete", progress="Simulation complete", result=result, message="")
        except subprocess.CalledProcessError as exc:
            captured = "\n".join(part for part in (exc.stdout, exc.stderr) if part)
            detail_log = f"returncode: {exc.returncode}\ncommand: {exc.cmd!r}\n{captured}"
            (folder / "failure-diagnostics.log").write_text(detail_log[-512 * 1024:] + "\n", encoding="utf-8")
            (folder / "failure-traceback.log").write_text(traceback.format_exc(), encoding="utf-8")
            detail = captured.strip().splitlines()
            message = detail[-1][-260:] if detail else f"RampLab command failed (exit {exc.returncode}); diagnostics saved with this run."
            self._update(ident, status="failed", progress="Simulation failed", message=message, diagnostics_available=True)
        except (OSError, ValueError, TypeError, KeyError, GenerationError, RuntimeError, subprocess.TimeoutExpired) as exc:
            (folder / "failure-traceback.log").write_text(traceback.format_exc(), encoding="utf-8")
            self._update(ident, status="failed", progress="Simulation failed", message=str(exc)[:400])


def _resolve_cli(build: Path | None = None) -> Path:
    build = build or ROOT / "build-goal30"
    return next((path for path in (build / "Release" / "airside_cli.exe", build / "airside_cli.exe", build / "airside_cli") if path.is_file()), build / "Release" / "airside_cli.exe")


def catalog() -> dict[str, Any]:
    raw = json.loads((RELEASE / "scenario_catalog.json").read_text(encoding="utf-8"))
    return {"schema_version": raw.get("schema_version", 1), "scenarios": raw.get("scenarios", {}),
            "available_bases": [
                {"key": "kauo", "label": "KAUO · KAUO calibrated prototype", "provenance": "FAA runway references; approximate taxiway and apron geometry"},
                {"key": "kauo-goal27", "label": "KAUO · two-aircraft operations prototype", "provenance": "Goal 27 scenario assumptions; approximate taxiway and apron geometry"},
                {"key": "synthetic", "label": "Synthetic operations example", "provenance": "Synthetic regression fixture; not airport data"},
            ]}


def bootstrap(key: str = "kauo") -> dict[str, Any]:
    data, mapping, geometry = load_base(key)
    scenario = new_scenario(key)
    geo = geometry["airport"]
    meta = data["airport"]
    return {"airport": {"id": meta.get("airport_id"), "name": meta.get("name"), "iata": meta.get("iata"),
                        "timezone": meta.get("local_timezone", "UTC"), "dataset_id": data["dataset"].get("dataset_id"),
                        "dataset_description": data["dataset"].get("description"), "validation": data["validation"],
                        "runways": meta.get("runways", [])},
            "geometry": {"nodes": geo.get("nodes", []), "edges": geo.get("edges", []), "features": geo.get("features", {}),
                         "gates": geometry.get("gates", []), "surface_operations": geometry.get("surface_operations", {})},
            "capabilities": {"mapped_routes": mapping.get("route_ids", {}), "vehicle_types": mapping.get("equipment_types", {}),
                             "services": mapping.get("service_types", {})},
            "scenario": scenario, "catalog": catalog(), "saved": ScenarioStore().list(),
            "runs": RunManagerSingleton.list()}


def compare_runs(baseline_id: str, variant_id: str) -> dict[str, Any]:
    records = {item["id"]: item for item in RunManagerSingleton.list()}
    if baseline_id not in records or variant_id not in records:
        raise ValueError("Choose two completed runs to compare.")
    left, right = records[baseline_id], records[variant_id]
    if left.get("status") != "complete" or right.get("status") != "complete":
        raise ValueError("Both runs must finish before they can be compared.")
    analyzer_path = ROOT / "tools" / "experiment_analysis" / "analyze.py"
    spec = importlib.util.spec_from_file_location("ramplab_studio_analyzer", analyzer_path)
    if spec is None or spec.loader is None:
        raise RuntimeError("The RampLab analysis module could not be loaded.")
    analyzer = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = analyzer
    spec.loader.exec_module(analyzer)
    left_run = analyzer.load_run(Path(left["result"]["folder"]) / "result" / "experiment.json")
    right_run = analyzer.load_run(Path(right["result"]["folder"]) / "result" / "experiment.json")
    return analyzer.compare(left_run, right_run)


def export_generated(scenario: dict[str, Any]) -> bytes:
    checked = validate_scenario(scenario)
    if not checked["valid"]:
        raise ValueError("Fix the scenario validation errors before exporting.")
    with __import__("tempfile").TemporaryDirectory(prefix="ramplab-studio-export-") as temp:
        root = Path(temp)
        package = root / "canonical.json"
        package.write_text(json.dumps(scenario_package(scenario), ensure_ascii=False, sort_keys=True, indent=2) + "\n", encoding="utf-8")
        generated = root / "generated"
        generate(package, _mapping(scenario.get("airport_key", "kauo")), generated, seed=int(scenario.get("seed", 42)))
        validate_generated(generated)
        buffer = BytesIO()
        with ZipFile(buffer, "w", ZIP_DEFLATED) as archive:
            for filename in ("scenario.json", "manifest.json", "identity-map.json", "support-matrix.json"):
                archive.write(generated / filename, filename)
        return buffer.getvalue()


def export_run(ident: str) -> bytes:
    run = RunManagerSingleton.get(ident)
    if run.get("status") != "complete":
        raise ValueError("Run artifacts are available after the simulation completes.")
    root = Path(run["folder"])
    buffer = BytesIO()
    with ZipFile(buffer, "w", ZIP_DEFLATED) as archive:
        for relative in ("canonical.json", "generated/scenario.json", "generated/manifest.json", "generated/identity-map.json",
                         "generated/support-matrix.json", "result/simulator-metrics.json", "result/simulator-metrics.csv",
                         "result/events.jsonl", "result/experiment.json", "result/runs.csv", "result/aircraft.csv", "result/release-run.json"):
            path = root / relative
            if path.is_file():
                archive.write(path, relative.replace("\\", "/"))
    return buffer.getvalue()


def launch_viewer(ident: str) -> dict[str, Any]:
    run = RunManagerSingleton.get(ident)
    if run.get("status") != "complete":
        raise ValueError("Finish the simulation before opening it in the Unreal viewer.")
    generated = Path(run["folder"]) / "generated" / "scenario.json"
    if not generated.is_file():
        raise FileNotFoundError("The generated RampLab scenario file is missing.")
    from tools.release.ramplab import launch_unreal
    pid = launch_unreal(Path(run["folder"]) / "unreal-windowed.log", scenario_file=generated)
    return {"pid": pid, "message": "The Goal 29 viewer was asked to open this generated scenario."}


ScenarioStoreSingleton = ScenarioStore()
RunManagerSingleton = RunManager()
