"""Persistent operational dataset, schedule, replay, scenario and live-session APIs."""
from __future__ import annotations

from copy import deepcopy
from datetime import datetime, timedelta, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import threading
import uuid
from typing import Any
from zipfile import ZIP_DEFLATED, ZipFile
from io import BytesIO

try:
    from . import operational_replay as replay_module
    from . import operational_schedule as schedule_module
    from .operational_time import iso_utc, parse_utc, utc_now, zone
except ImportError:  # Support the dashboard's existing top-level script/test imports.
    import operational_replay as replay_module
    import operational_schedule as schedule_module
    from operational_time import iso_utc, parse_utc, utc_now, zone


class OperationalStoreError(ValueError):
    pass


class OperationalStore:
    """A local, versioned store; source imports stay immutable after ingestion."""
    def __init__(self, home: Path, studio: Any):
        self.root = Path(home).resolve() / "operational-data"
        self.profiles = self.root / "profiles"
        self.datasets = self.root / "datasets"
        self.replays = self.root / "replays"
        self.scenarios = self.root / "scenarios"
        self.live = self.root / "live-sessions"
        self.studio = studio
        self._lock = threading.RLock()
        self._clocks: dict[str, replay_module.ReplayClock] = {}
        for path in (self.profiles, self.datasets, self.replays, self.scenarios, self.live):
            path.mkdir(parents=True, exist_ok=True)
        built_in = Path(__file__).parent / "fixtures" / "operational" / "kauo-demo-schedule-profile.json"
        try:
            profile = json.loads(built_in.read_text(encoding="utf-8"))
            destination = self._profile_path(profile["profile_id"])
            if not destination.exists(): self._write(destination, profile)
        except (OSError, ValueError, KeyError, TypeError):
            pass

    @staticmethod
    def _json(path: Path) -> Any:
        try: return json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc: raise OperationalStoreError(f"Could not read {path.name}: {exc}") from None

    @staticmethod
    def _write(path: Path, data: Any) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        temp = path.with_name(path.name + f".{os.getpid()}.{threading.get_ident()}.tmp")
        temp.write_text(json.dumps(data, ensure_ascii=False, sort_keys=True, indent=2, allow_nan=False) + "\n", encoding="utf-8")
        os.replace(temp, path)

    @staticmethod
    def _safe_id(value: str, pattern: str = r"[A-Za-z0-9][A-Za-z0-9_-]{0,100}") -> str:
        if not isinstance(value, str) or not re.fullmatch(pattern, value):
            raise OperationalStoreError("Invalid operational data identifier.")
        return value

    def _profile_path(self, ident: str) -> Path:
        return self.profiles / (self._safe_id(ident) + ".json")

    def _dataset_path(self, ident: str) -> Path:
        return self.datasets / self._safe_id(ident, r"ops_[a-f0-9]{20}")

    def _replay_path(self, ident: str) -> Path:
        return self.replays / self._safe_id(ident, r"replay_[a-f0-9]{20}")

    def _scenario_path(self, ident: str) -> Path:
        return self.scenarios / self._safe_id(ident, r"scenario_[a-f0-9]{20}")

    def _session_path(self, ident: str) -> Path:
        return self.live / self._safe_id(ident, r"live_[A-Za-z0-9_-]{1,100}")

    def list_profiles(self) -> list[dict[str, Any]]:
        rows = []
        for path in self.profiles.glob("*.json"):
            try:
                profile = self._json(path)
                rows.append({"profile_id": profile["profile_id"], "name": profile.get("name", profile["profile_id"]),
                             "airport_id": profile["airport_id"], "timezone": profile["timezone"],
                             "operational_date": profile.get("operational_date"), "source_format": profile.get("source_format")})
            except (KeyError, OperationalStoreError): continue
        return sorted(rows, key=lambda row: row["name"].casefold())

    def get_profile(self, ident: str) -> dict[str, Any]:
        path = self._profile_path(ident)
        profile = self._json(path)
        schedule_module.validate_profile(profile)
        return profile

    def save_profile(self, profile: dict[str, Any]) -> dict[str, Any]:
        schedule_module.validate_profile(profile)
        ident = self._safe_id(profile["profile_id"])
        self._write(self._profile_path(ident), profile)
        return self.get_profile(ident)

    def preview_schedule(self, filename: str, content: str) -> dict[str, Any]:
        if not isinstance(filename, str) or not isinstance(content, str):
            raise OperationalStoreError("Provide a filename and text file contents.")
        if len(content.encode("utf-8")) > 8 * 1024 * 1024:
            raise OperationalStoreError("Schedule uploads are limited to 8 MiB.")
        return schedule_module.preview(Path(filename.replace("\\", "/")).name, content)

    def import_schedule(self, filename: str, content: str, profile: dict[str, Any]) -> dict[str, Any]:
        preview = self.preview_schedule(filename, content)
        data = schedule_module.import_schedule(preview["filename"], content, profile)
        folder = self._dataset_path(data["dataset_id"])
        folder.mkdir(parents=True, exist_ok=True)
        self._write(folder / "normalized-schedule.json", data)
        self._write(folder / "mapping-profile.json", profile)
        (folder / f"source.{preview['format']}").write_bytes(content.encode("utf-8"))
        scenario_ready = False
        scenario_findings = []
        if not data["quality_summary"]["errors"]:
            try:
                package = schedule_module.validate_through_goal24a(data)
                self._write(folder / "goal24a-canonical.json", package)
                scenario_ready = True
            except (schedule_module.ScheduleImportError, OSError, ValueError, KeyError) as exc:
                scenario_findings.append({"severity": "ERROR", "code": "SCENARIO_GENERATION_BLOCKED", "message": str(exc)})
        data["scenario_ready"] = scenario_ready
        data["scenario_findings"] = scenario_findings
        self._write(folder / "normalized-schedule.json", data)
        return data

    def list_datasets(self) -> list[dict[str, Any]]:
        rows = []
        for folder in self.datasets.iterdir():
            if not folder.is_dir(): continue
            try:
                data = self._json(folder / "normalized-schedule.json")
                rows.append({"dataset_id": data["dataset_id"], "airport_id": data["airport_id"],
                             "operational_date": data["operational_date"], "timezone": data["timezone"],
                             "flight_count": len(data["flights"]), "status": data["quality_summary"]["status"],
                             "scenario_ready": data.get("scenario_ready", False),
                             "source_filename": data["provenance"]["source_filename"],
                             "source_sha256": data["provenance"]["source_sha256"]})
            except (OperationalStoreError, KeyError, TypeError): continue
        return sorted(rows, key=lambda row: (row.get("operational_date") or "", row["dataset_id"]), reverse=True)

    def get_dataset(self, ident: str) -> dict[str, Any]:
        return self._json(self._dataset_path(ident) / "normalized-schedule.json")

    def get_dataset_export(self, ident: str) -> bytes:
        folder = self._dataset_path(ident)
        normalized = self._json(folder / "normalized-schedule.json")
        buffer = BytesIO()
        with ZipFile(buffer, "w", ZIP_DEFLATED) as archive:
            archive.write(folder / "normalized-schedule.json", "normalized-schedule.json")
            archive.write(folder / "mapping-profile.json", "mapping-profile.json")
            archive.write(folder / f"source.{normalized['provenance']['source_format']}", "source-schedule." + normalized["provenance"]["source_format"])
            if (folder / "goal24a-canonical.json").is_file(): archive.write(folder / "goal24a-canonical.json", "goal24a-canonical.json")
            archive.writestr("plan-vs-actual.json", json.dumps(schedule_module.plan_vs_actual(normalized, self._events_for_dataset(ident)), ensure_ascii=False, indent=2) + "\n")
        return buffer.getvalue()

    def _dataset_replay_findings(self, dataset: dict[str, Any], replay: dict[str, Any]) -> list[dict[str, Any]]:
        findings = deepcopy(replay.get("findings", []))
        known_flights = {row["flight_id"] for row in dataset.get("flights", [])}
        known_aircraft = {row["aircraft_id"] for row in dataset.get("aircraft", [])}
        start_day = dataset.get("operational_date")
        from datetime import date
        local_date = date.fromisoformat(start_day) if start_day else None
        for event in replay.get("events", []):
            kind, entity = event["event_type"], event["entity_id"]
            if kind in {"vehicle_status"}:
                known = known_aircraft | {str(row.get("equipment_id")) for row in dataset.get("equipment", [])}
            elif kind in {"closure_started", "closure_ended"}:
                known = None
            else:
                known = known_flights
            if known is not None and entity not in known:
                findings.append({"severity": "WARNING", "code": "EVENT_ENTITY_UNKNOWN", "message": f"Event {event['event_id']} references unknown operational entity {entity!r}.",
                                 "source": event["provenance"]["filename"], "row": event["provenance"]["row"], "field": "entity_id", "value": entity})
            if local_date:
                event_day = replay_module._timestamp(event["timestamp"]).astimezone(zone(dataset["timezone"])).date()
                if abs((event_day - local_date).days) > 1:
                    findings.append({"severity": "WARNING", "code": "EVENT_OUTSIDE_OPERATIONAL_WINDOW", "message": f"Event {event['event_id']} falls outside the operational date and adjacent rollover day.",
                                     "source": event["provenance"]["filename"], "row": event["provenance"]["row"], "field": "timestamp", "value": event["timestamp"]})
        return sorted(findings, key=lambda item: (item["severity"], item["code"], item.get("row") or 0, item.get("field") or ""))

    def import_events(self, dataset_id: str, filename: str, content: str) -> dict[str, Any]:
        dataset = self.get_dataset(dataset_id)
        raw = content.encode("utf-8")
        replay = replay_module.parse_events(Path(filename.replace("\\", "/")).name, raw, dataset["timezone"])
        replay["dataset_id"] = dataset_id
        replay["findings"] = self._dataset_replay_findings(dataset, replay)
        replay["replay_id"] = "replay_" + hashlib.sha256((dataset_id + replay["normalized_sha256"]).encode()).hexdigest()[:20]
        replay["provenance"] = {"source_sha256": replay["source_sha256"], "normalized_sha256": replay["normalized_sha256"],
                                "dataset_id": dataset_id, "timezone": dataset["timezone"], "ingested_at": utc_now(),
                                "schema_version": replay_module.SCHEMA_VERSION}
        if dataset.get("provenance", {}).get("data_classification") == "SYNTHETIC":
            replay["provenance"]["data_classification"] = "SYNTHETIC"
        try:
            source_document = json.loads(content)
            if isinstance(source_document, dict) and "synthetic" in str(source_document.get("provenance", "")).casefold():
                replay["provenance"]["data_classification"] = "SYNTHETIC"
        except (json.JSONDecodeError, TypeError):
            pass
        path = self._replay_path(replay["replay_id"])
        path.mkdir(parents=True, exist_ok=True)
        self._write(path / "replay.json", replay)
        (path / f"source.{Path(filename).suffix.lower().lstrip('.')}").write_bytes(raw)
        timeline = replay_module.replay_timeline(dataset, replay["events"])
        self._write(path / "timeline.json", timeline)
        return replay

    def list_replays(self, dataset_id: str | None = None) -> list[dict[str, Any]]:
        rows = []
        for folder in self.replays.iterdir():
            if not folder.is_dir(): continue
            try:
                replay = self._json(folder / "replay.json")
                if dataset_id and replay.get("dataset_id") != dataset_id: continue
                rows.append({"replay_id": replay["replay_id"], "dataset_id": replay["dataset_id"],
                             "event_count": replay["event_count"], "normalized_sha256": replay["normalized_sha256"],
                             "source_filename": replay["source_filename"], "findings": replay.get("findings", [])})
            except (OperationalStoreError, KeyError): continue
        return sorted(rows, key=lambda row: row["replay_id"])

    def get_replay(self, replay_id: str) -> dict[str, Any]:
        return self._json(self._replay_path(replay_id) / "replay.json")

    def replay_snapshot(self, replay_id: str) -> dict[str, Any]:
        with self._lock:
            replay = self.get_replay(replay_id)
            if replay_id not in self._clocks:
                dataset = self.get_dataset(replay["dataset_id"])
                self._clocks[replay_id] = replay_module.ReplayClock(dataset, replay_module.replay_timeline(dataset, replay["events"]))
            clock = self._clocks[replay_id]
            snapshot = clock.snapshot()
            snapshot["replay_id"] = replay_id
            snapshot["events"] = replay["events"]
            snapshot["findings"] = replay.get("findings", [])
            snapshot["freshness"] = replay_module.freshness(replay["events"])
            snapshot["plan_vs_actual"] = schedule_module.plan_vs_actual(self.get_dataset(replay["dataset_id"]), replay["events"])
            snapshot["aircraft_history"] = {ident: deepcopy(row["history"]) for ident, row in snapshot["state"]["flights"].items()}
            return snapshot

    def control_replay(self, replay_id: str, action: str, value: Any = None) -> dict[str, Any]:
        self.replay_snapshot(replay_id)
        with self._lock:
            clock = self._clocks[replay_id]
            if action == "play": result = clock.play(value)
            elif action == "pause": result = clock.pause()
            elif action == "step": result = clock.step()
            elif action == "seek": result = clock.seek(value)
            elif action == "advance": result = clock.advance(value)
            else: raise OperationalStoreError("Replay action must be play, pause, step, seek, or advance.")
        return self.replay_snapshot(replay_id)

    def _events_for_dataset(self, dataset_id: str) -> list[dict[str, Any]]:
        events = []
        for item in self.list_replays(dataset_id):
            events.extend(self.get_replay(item["replay_id"])["events"])
        unique = {event["event_id"]: event for event in events}
        return sorted(unique.values(), key=lambda event: (event["timestamp"], event["event_id"]))

    def plan_actual(self, dataset_id: str, replay_id: str | None = None) -> dict[str, Any]:
        data = self.get_dataset(dataset_id)
        events = self.get_replay(replay_id)["events"] if replay_id else self._events_for_dataset(dataset_id)
        return {"dataset_id": dataset_id, "classification": "OBSERVED_AND_SCHEDULED",
                "rows": schedule_module.plan_vs_actual(data, events), "provenance": data["provenance"]}

    def create_plan_scenario(self, dataset_id: str) -> dict[str, Any]:
        data = self.get_dataset(dataset_id)
        canonical = schedule_module.validate_through_goal24a(data)
        profile = self.get_profile(data["provenance"]["mapping_profile"]["profile_id"])
        mapping_key = profile.get("ramp_mapping_key")
        if not mapping_key:
            raise OperationalStoreError("This profile has no configured Goal 24B airport mapping key.")
        scenario = self.studio.new_scenario(mapping_key)
        scenario.update({"name": f"Plan · {data['airport_id']} · {data['operational_date']}",
                         "id": "", "dataset_id": data["dataset_id"], "baseline": True,
                         "flights": deepcopy(canonical["flights"]), "aircraft": deepcopy(canonical["aircraft"]),
                         "gates": deepcopy(canonical["gates"]), "equipment": [],
                         "turnaround_requirements": deepcopy(canonical["turnaround_requirements"]), "disruptions": [],
                         "operational_provenance": deepcopy(canonical["operational_provenance"]),
                         "operational_schedule": {"operational_date": data["operational_date"], "timezone": data["timezone"],
                                                  "classification": "SCHEDULED", "dataset_id": data["dataset_id"]},
                         "scenario_assumptions": [
                             {"field": "airport geometry and route graph", "classification": "ASSUMED",
                              "source": f"Goal 24B mapping {mapping_key}; geometry is the configured simulator representation."},
                            {"field": "initial aircraft position and taxi routing", "classification": "SIMULATED",
                             "source": "RampLab Goal 24B uses its existing arrival/departure nodes and routing behavior."},
                            {"field": "turnaround service requirements", "classification": "ASSUMED" if canonical["turnaround_requirements"] else "UNKNOWN",
                             "source": ("Explicit service-task assumptions in the selected mapping profile; operational requirements remain unverified." if canonical["turnaround_requirements"] else "The schedule and mapping profile do not provide service tasks; the simulator Plan cannot run until an explicit profile assumption is configured.")},
                            {"field": "aircraft dimensions and stand details", "classification": "UNKNOWN",
                              "source": "Not supplied by the imported schedule profile."}]})
        validation = self.studio.validate_scenario(scenario)
        configured_aircraft = {row["aircraft_id"] for row in canonical["turnaround_requirements"]}
        departing_aircraft = {flight["aircraft_id"] for flight in canonical["flights"] if flight["operation"] == "departure"}
        missing_service_assumptions = sorted(departing_aircraft - configured_aircraft)
        if missing_service_assumptions:
            validation["findings"].extend({"severity": "error", "code": "TURNAROUND_SERVICES_UNKNOWN",
                "message": f"No turnaround service requirements or explicit profile assumptions are configured for aircraft {ident!r}."}
                for ident in missing_service_assumptions)
            validation["valid"] = False
            validation["status"] = "needs_attention"
        scenario["validation"] = validation
        if not validation.get("valid"):
            return {"scenario": scenario, "validation": validation, "scenario_id": None}
        digest_view = deepcopy(scenario); digest_view.pop("created_at", None); digest_view.pop("validation", None)
        scenario_id = "scenario_" + hashlib.sha256(schedule_module.canonical_bytes(digest_view)).hexdigest()[:20]
        scenario["id"] = scenario_id
        self._write(self._scenario_path(scenario_id) / "scenario.json", scenario)
        return {"scenario": scenario, "validation": validation, "scenario_id": scenario_id}

    def get_scenario(self, scenario_id: str) -> dict[str, Any]:
        return self._json(self._scenario_path(scenario_id) / "scenario.json")

    def create_counterfactual(self, dataset_id: str, point_in_time: str, route_id: str,
                              delay_minutes: int = 20, duration_minutes: int = 30,
                              replay_id: str | None = None) -> dict[str, Any]:
        if isinstance(delay_minutes, bool) or not isinstance(delay_minutes, int) or not 0 <= delay_minutes <= 1440:
            raise OperationalStoreError("Counterfactual intervention delay must be 0 through 1,440 minutes.")
        if isinstance(duration_minutes, bool) or not isinstance(duration_minutes, int) or not 1 <= duration_minutes <= 1440:
            raise OperationalStoreError("Counterfactual closure duration must be 1 through 1,440 minutes.")
        start = parse_utc(point_in_time, "UTC") + timedelta(minutes=delay_minutes)
        end = start + timedelta(minutes=duration_minutes)
        plan = self.create_plan_scenario(dataset_id)
        scenario = plan["scenario"]
        if not plan["scenario_id"]:
            raise OperationalStoreError("The imported plan cannot generate a scenario: " + "; ".join(row["message"] for row in plan["validation"].get("findings", []) if row.get("severity") == "error"))
        mapping = self.studio.load_mapping(self.studio._mapping(scenario["airport_key"]))[0]
        if route_id not in mapping.get("route_ids", {}):
            raise OperationalStoreError("Counterfactual route is not explicitly mapped to simulator geometry.")
        replay_events = self.get_replay(replay_id)["events"] if replay_id else self._events_for_dataset(dataset_id)
        actual_by_flight: dict[str, dict[str, str]] = {}
        for event in replay_events:
            target = actual_by_flight.setdefault(event["entity_id"], {})
            if event["event_type"] in {"actual_arrival", "actual_departure"}:
                target[event["event_type"]] = event["timestamp"]
        for flight in scenario["flights"]:
            source_flight = flight.get("flight_number", "").rsplit("::", 1)[0]
            actual = actual_by_flight.get(source_flight, {})
            if flight["operation"] == "arrival" and actual.get("actual_arrival"):
                flight["scheduled_time"] = actual["actual_arrival"]
            if flight["operation"] == "departure" and actual.get("actual_departure"):
                flight["scheduled_time"] = actual["actual_departure"]
        disruption = {"disruption_id": "GOAL33-COUNTERFACTUAL-CLOSURE", "type": "route_closure",
                      "resource_id": route_id, "enabled": True, "start_time": iso_utc(start),
                      "end_time": iso_utc(end), "reason": "Counterfactual simulation intervention"}
        scenario["disruptions"] = [disruption]
        scenario["baseline"] = False
        scenario["name"] = f"Counterfactual · {scenario['name']}"
        scenario["counterfactual"] = {"classification": "SIMULATED", "dataset_id": dataset_id,
                                       "replay_id": replay_id, "intervention_point": iso_utc(parse_utc(point_in_time, "UTC")),
                                       "route_id": route_id, "start_time": iso_utc(start),
                                       "delay_minutes": delay_minutes, "duration_minutes": duration_minutes,
                                       "execution_note": "RampLab re-simulates the operational-day scenario with observed arrival/departure timestamps where available and applies the intervention at the selected replay point. Intermediate state is not restored from the historical feed."}
        validation = self.studio.validate_scenario(scenario)
        scenario["validation"] = validation
        if not validation.get("valid"):
            return {"scenario": scenario, "scenario_id": None, "validation": validation}
        digest_view = deepcopy(scenario); digest_view.pop("created_at", None); digest_view.pop("validation", None); digest_view.pop("id", None)
        scenario_id = "scenario_" + hashlib.sha256(schedule_module.canonical_bytes(digest_view)).hexdigest()[:20]
        scenario["id"] = scenario_id
        self._write(self._scenario_path(scenario_id) / "scenario.json", scenario)
        return {"scenario": scenario, "scenario_id": scenario_id, "validation": validation}

    def start_live_session(self, dataset_id: str) -> dict[str, Any]:
        self.get_dataset(dataset_id)
        for path in self.live.glob("*/session.json"):
            try:
                item = self._json(path)
                if item.get("dataset_id") == dataset_id and item.get("status") == "OPEN":
                    return self.live_state(item["session_id"])
            except OperationalStoreError: continue
        session_id = "live_" + dataset_id[4:] + "_" + uuid.uuid4().hex[:8]
        folder = self._session_path(session_id); folder.mkdir(parents=True, exist_ok=True)
        session = {"schema_version": "1.0", "session_id": session_id, "dataset_id": dataset_id,
                   "started_at": utc_now(), "status": "OPEN", "event_count": 0,
                   "event_file": "events.jsonl", "provenance": {"classification": "LIVE_OBSERVATION", "source": "local update interface"}}
        self._write(folder / "session.json", session)
        (folder / "events.jsonl").touch(exist_ok=True)
        return self.live_state(session_id)

    def _session_events(self, session: dict[str, Any]) -> list[dict[str, Any]]:
        path = self._session_path(session["session_id"]) / "events.jsonl"
        events = []
        if path.is_file():
            for line in path.read_text(encoding="utf-8").splitlines():
                if line.strip(): events.append(json.loads(line))
        return events

    def ingest_live_event(self, body: dict[str, Any], *, received_at: str | None = None) -> dict[str, Any]:
        if not isinstance(body, dict) or body.get("schema_version") != "1.0":
            raise OperationalStoreError("Live events require schema_version 1.0.")
        session_id = self._safe_id(body.get("session_id", ""), r"live_[A-Za-z0-9_-]{1,100}")
        with self._lock:
            folder = self._session_path(session_id)
            session = self._json(folder / "session.json")
            if session.get("status") != "OPEN": raise OperationalStoreError("This live-update session is closed.")
            dataset = self.get_dataset(session["dataset_id"])
            profile = self.get_profile(dataset["provenance"]["mapping_profile"]["profile_id"])
            event = {key: body[key] for key in ("event_id", "event_type", "type", "timestamp", "entity_id", "flight_id", "aircraft_id", "equipment_id", "payload", "source", "confidence") if key in body}
            if not event.get("source"): event["source"] = "local-live-update"
            event["operational_date"] = dataset.get("operational_date")
            parsed = replay_module.parse_events("live-event.json", json.dumps([event]), dataset["timezone"], received_at=received_at or utc_now())
            normalized_event = parsed["events"][0]
            existing = self._session_events(session)
            previous = next((item for item in existing if item["event_id"] == normalized_event["event_id"]), None)
            if previous:
                if replay_module._event_identity(previous) != replay_module._event_identity(normalized_event):
                    raise OperationalStoreError("Idempotency key was already used with different event content.")
                return {"accepted": True, "duplicate": True, "late": False, "event": previous,
                        "state": self.live_state(session_id)}
            flights = {row["flight_id"] for row in dataset.get("flights", [])}
            equipment_ids = {row.get("equipment_id") for row in dataset.get("equipment", [])}
            kind, entity = normalized_event["event_type"], normalized_event["entity_id"]
            if kind not in {"closure_started", "closure_ended"} and entity not in flights and entity not in equipment_ids:
                raise OperationalStoreError(f"Live event references unknown entity {entity!r}.")
            normalized_event["received_at"] = received_at or utc_now()
            late = bool(existing and normalized_event["timestamp"] < max(item["timestamp"] for item in existing))
            with (folder / "events.jsonl").open("a", encoding="utf-8", newline="\n") as handle:
                handle.write(json.dumps(normalized_event, ensure_ascii=False, sort_keys=True, separators=(",", ":")) + "\n")
            session["event_count"] = len(existing) + 1
            session["last_received_at"] = normalized_event["received_at"]
            session["late_event_count"] = session.get("late_event_count", 0) + int(late)
            self._write(folder / "session.json", session)
            return {"accepted": True, "duplicate": False, "late": late, "event": normalized_event,
                    "state": self.live_state(session_id)}

    def live_state(self, session_id: str) -> dict[str, Any]:
        session = self._json(self._session_path(session_id) / "session.json")
        dataset = self.get_dataset(session["dataset_id"])
        events = self._session_events(session)
        chronological = sorted(events, key=lambda event: (event["timestamp"], event["event_id"]))
        result = replay_module.replay_timeline(dataset, chronological)
        live_events = [{**event} for event in events]
        return {"session": session, "dataset_id": dataset["dataset_id"], "operational_time": result["final_state"]["observed_at"],
                "state": result["final_state"], "events": sorted(live_events, key=lambda event: (event.get("received_at", ""), event["event_id"]), reverse=True)[:12],
                "freshness": replay_module.freshness(events), "replay_sha256": result["normalized_sha256"],
                "data_classification": "OBSERVED", "simulation_state": None}

    def close_live_session(self, session_id: str) -> dict[str, Any]:
        folder = self._session_path(session_id)
        session = self._json(folder / "session.json")
        if session.get("status") == "OPEN":
            session["status"] = "CLOSED"; session["closed_at"] = utc_now()
            self._write(folder / "session.json", session)
        return self.live_state(session_id)

    def list_live_sessions(self, dataset_id: str | None = None) -> list[dict[str, Any]]:
        rows = []
        for path in self.live.glob("*/session.json"):
            try:
                session = self._json(path)
                if not dataset_id or session.get("dataset_id") == dataset_id:
                    rows.append({key: session.get(key) for key in ("session_id", "dataset_id", "started_at", "closed_at", "status", "event_count", "late_event_count")})
            except OperationalStoreError: continue
        return sorted(rows, key=lambda row: row["started_at"] or "", reverse=True)

    def recorded_replay(self, session_id: str) -> dict[str, Any]:
        state = self.live_state(session_id)
        session = state["session"]
        if session.get("status") != "CLOSED": raise OperationalStoreError("Close the live-update session before recording it as replay history.")
        path = self._session_path(session_id) / "events.jsonl"
        replay = replay_module.parse_events("recorded-session.jsonl", path.read_bytes(), self.get_dataset(session["dataset_id"])["timezone"])
        return self.import_events(session["dataset_id"], "recorded-session.jsonl", path.read_text(encoding="utf-8"))

    def export_replay(self, replay_id: str) -> bytes:
        folder = self._replay_path(replay_id)
        buffer = BytesIO()
        with ZipFile(buffer, "w", ZIP_DEFLATED) as archive:
            for name in ("replay.json", "timeline.json"):
                archive.write(folder / name, name)
            for path in folder.glob("source.*"): archive.write(path, "source-events" + path.suffix)
        return buffer.getvalue()

    def export_session(self, session_id: str) -> bytes:
        folder = self._session_path(session_id)
        buffer = BytesIO()
        with ZipFile(buffer, "w", ZIP_DEFLATED) as archive:
            for name in ("session.json", "events.jsonl"):
                archive.write(folder / name, name)
            state = self.live_state(session_id)
            archive.writestr("state.json", json.dumps(state, ensure_ascii=False, sort_keys=True, indent=2) + "\n")
        return buffer.getvalue()
