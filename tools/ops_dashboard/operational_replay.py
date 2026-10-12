"""Timestamped operational event ingestion and deterministic replay primitives."""
from __future__ import annotations

import csv
import hashlib
import io
import json
from copy import deepcopy
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

try:
    from .operational_schedule import canonical_bytes, sha256
    from .operational_time import OperationalTimeError, iso_utc, parse_utc, utc_now
except ImportError:  # Support the dashboard's existing top-level script/test imports.
    from operational_schedule import canonical_bytes, sha256
    from operational_time import OperationalTimeError, iso_utc, parse_utc, utc_now

SCHEMA_VERSION = "1.0"
EVENT_TYPES = {"actual_arrival", "stand_arrival", "service_started", "service_completed", "pushback",
               "actual_departure", "vehicle_status", "equipment_status", "closure_started", "closure_ended",
               "closure_cleared", "gate_changed", "flight_delayed", "flight_delay"}
SPEEDS = {1, 5, 10, 60}


class ReplayError(ValueError):
    pass


def _timestamp(value: Any) -> datetime:
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        return datetime.fromtimestamp(value, timezone.utc)
    try:
        return parse_utc(value, "UTC")
    except OperationalTimeError as exc:
        raise ReplayError(f"Invalid event timestamp: {exc}") from exc


def _normalize_event(event: dict[str, Any], timezone_name: str, filename: str, row: int,
                     received_at: str | None = None) -> dict[str, Any]:
    event_id = str(event.get("event_id", "")).strip()
    event_type = str(event.get("event_type", event.get("type", ""))).strip().casefold()
    event_type = {"equipment_status": "vehicle_status", "closure_cleared": "closure_ended"}.get(event_type, event_type)
    entity_id = str(event.get("entity_id", event.get("flight_id", event.get("aircraft_id", event.get("equipment_id", ""))))).strip()
    if not event_id: raise ReplayError(f"Row {row}: event_id is required.")
    if event_type not in EVENT_TYPES: raise ReplayError(f"Row {row}: unsupported event type {event_type!r}.")
    if not entity_id: raise ReplayError(f"Row {row}: entity_id is required.")
    raw_timestamp = event.get("timestamp", event.get("event_timestamp"))
    try:
        timestamp = iso_utc(parse_utc(raw_timestamp, timezone_name, timestamp_format=event.get("timestamp_format")))
    except OperationalTimeError as exc:
        raise ReplayError(f"Row {row}: {exc}") from exc
    payload = event.get("payload", {})
    if isinstance(payload, str):
        try: payload = json.loads(payload)
        except json.JSONDecodeError as exc: raise ReplayError(f"Row {row}: payload must be valid JSON.") from exc
    if not isinstance(payload, dict): raise ReplayError(f"Row {row}: payload must be an object.")
    required_payload = {"gate_changed": "stand_id", "stand_arrival": "stand_id", "vehicle_status": "status",
                        "closure_started": "disruption_id", "closure_ended": "disruption_id",
                        "flight_delayed": "delay_minutes"}
    if event_type in required_payload:
        required = required_payload[event_type]
        if required not in payload or payload[required] in (None, ""):
            raise ReplayError(f"Row {row}: {event_type} requires payload.{required}.")
    if event_type == "flight_delay":
        if "new_departure" in payload:
            try:
                payload["new_departure"] = iso_utc(parse_utc(payload["new_departure"], timezone_name, operational_date=event.get("operational_date")))
            except OperationalTimeError as exc:
                raise ReplayError(f"Row {row}: payload.new_departure is invalid: {exc}") from exc
        elif "delay_minutes" not in payload:
            raise ReplayError(f"Row {row}: flight_delay requires payload.new_departure or payload.delay_minutes.")
    if event_type in {"flight_delayed", "flight_delay"} and "delay_minutes" in payload:
        value = payload.get("delay_minutes")
        if isinstance(value, bool) or not isinstance(value, (int, float)) or value < 0:
            raise ReplayError(f"Row {row}: delay_minutes must be a nonnegative number.")
    confidence = event.get("confidence")
    if confidence is not None and (isinstance(confidence, bool) or not isinstance(confidence, (int, float)) or not 0 <= confidence <= 1):
        raise ReplayError(f"Row {row}: confidence must be between 0 and 1.")
    result = {"event_id": event_id, "event_type": event_type, "timestamp": timestamp,
              "entity_id": entity_id, "payload": deepcopy(payload),
              "source": str(event.get("source") or filename), "classification": "OBSERVED",
              "confidence": confidence, "data_quality": str(event.get("data_quality") or "VALID"),
              "provenance": {"filename": filename, "row": row}}
    if received_at:
        result["received_at"] = received_at
    return result


def parse_events(filename: str, content: str | bytes, timezone_name: str, *, received_at: str | None = None) -> dict[str, Any]:
    raw = content if isinstance(content, bytes) else content.encode("utf-8")
    suffix = Path(filename).suffix.casefold()
    try:
        text = raw.decode("utf-8-sig")
        if suffix == ".csv":
            reader = csv.DictReader(io.StringIO(text, newline=""), strict=True)
            if not reader.fieldnames or len(reader.fieldnames) != len(set(reader.fieldnames)):
                raise ReplayError("CSV event input requires unique headers.")
            rows = list(reader)
            for row in rows:
                if row.get("payload"):
                    row["payload"] = json.loads(row["payload"])
                if row.get("confidence"):
                    row["confidence"] = float(row["confidence"])
        elif suffix == ".json":
            data = json.loads(text)
            rows = data.get("events", data.get("records", [])) if isinstance(data, dict) else data
        elif suffix == ".jsonl":
            rows = [json.loads(line) for line in text.splitlines() if line.strip()]
        else:
            raise ReplayError("Historical events must be CSV, JSON, or JSONL.")
    except (UnicodeError, csv.Error, json.JSONDecodeError, ValueError) as exc:
        if isinstance(exc, ReplayError): raise
        raise ReplayError(f"{Path(filename).name}: {exc}") from None
    if not isinstance(rows, list) or any(not isinstance(row, dict) for row in rows):
        raise ReplayError("Event source must be an array of objects or a CSV table.")
    findings: list[dict[str, Any]] = []
    unique: dict[str, dict[str, Any]] = {}
    events = []
    for index, row in enumerate(rows, start=2 if suffix == ".csv" else 1):
        event = _normalize_event(row, timezone_name, Path(filename).name, index, received_at)
        previous = unique.get(event["event_id"])
        if previous:
            same = _event_identity(previous) == _event_identity(event)
            findings.append({"severity": "WARNING" if same else "ERROR", "code": "EVENT_DUPLICATE" if same else "EVENT_ID_COLLISION",
                             "message": (f"Duplicate event {event['event_id']} was ignored." if same else f"Event ID {event['event_id']} was reused with different content."),
                             "source": Path(filename).name, "row": index, "field": "event_id", "value": event["event_id"]})
            if not same: continue
            continue
        unique[event["event_id"]] = event
        events.append(event)
    flights = set()
    ordered = sorted(events, key=lambda event: (event["timestamp"], event["event_id"]))
    for event in ordered:
        if event["event_type"] not in {"vehicle_status", "closure_started", "closure_ended"}:
            flights.add(event["entity_id"])
    digest_events = [{key: event[key] for key in ("event_id", "event_type", "timestamp", "entity_id", "payload", "source", "confidence", "data_quality")} for event in ordered]
    return {"schema_version": SCHEMA_VERSION, "source_filename": Path(filename).name,
            "source_sha256": sha256(raw), "timezone": timezone_name,
            "events": ordered, "findings": findings,
            "normalized_sha256": sha256(canonical_bytes(digest_events)),
            "event_count": len(ordered), "duplicate_count": sum(item["code"] == "EVENT_DUPLICATE" for item in findings),
            "flight_entities": sorted(flights)}


def _event_identity(event: dict[str, Any]) -> dict[str, Any]:
    return {key: event.get(key) for key in ("event_id", "event_type", "timestamp", "entity_id", "payload", "source", "confidence", "data_quality")}


def _initial_state(dataset: dict[str, Any]) -> dict[str, Any]:
    flights = {}
    for flight in dataset.get("flights", []):
        flights[flight["flight_id"]] = {"flight_id": flight["flight_id"], "aircraft_id": flight["aircraft_id"],
                                        "status": flight.get("status") or "SCHEDULED",
                                        "status_classification": "SCHEDULED", "stand_id": flight.get("stand_id"),
                                        "stand_classification": "SCHEDULED" if flight.get("stand_id") else "UNKNOWN",
                                        "scheduled_arrival": flight.get("scheduled_arrival"),
                                        "scheduled_departure": flight.get("scheduled_departure"),
                                        "actual_arrival": flight.get("actual_arrival"),
                                        "actual_departure": flight.get("actual_departure"),
                                        "history": []}
    return {"flights": flights, "vehicles": {}, "closures": {}, "recent_events": [],
            "observed_at": None, "simulated_state": None}


def _apply_event(state: dict[str, Any], event: dict[str, Any]) -> None:
    kind, entity, payload = event["event_type"], event["entity_id"], event["payload"]
    observed = {"event_id": event["event_id"], "event_type": kind, "timestamp": event["timestamp"],
                "classification": "OBSERVED", "source": event["source"], "confidence": event["confidence"]}
    if kind == "vehicle_status":
        state["vehicles"].setdefault(entity, {}).update({"status": payload["status"], "classification": "OBSERVED", "event": observed})
    elif kind in {"closure_started", "closure_ended"}:
        state["closures"][str(payload["disruption_id"])] = {"active": kind == "closure_started", "classification": "OBSERVED", "event": observed, "payload": payload}
    elif entity in state["flights"]:
        flight = state["flights"][entity]
        if kind == "actual_arrival": flight["actual_arrival"] = event["timestamp"]; flight["status"] = "ARRIVED"
        elif kind == "stand_arrival": flight["stand_id"] = payload["stand_id"]; flight["status"] = "AT_STAND"
        elif kind == "gate_changed": flight["stand_id"] = payload["stand_id"]; flight["status"] = "AT_STAND"
        elif kind == "service_started": flight["status"] = "SERVICE_IN_PROGRESS"
        elif kind == "service_completed": flight["status"] = "SERVICE_COMPLETE"
        elif kind == "pushback": flight["status"] = "PUSHED_BACK"
        elif kind == "actual_departure": flight["actual_departure"] = event["timestamp"]; flight["status"] = "DEPARTED"
        elif kind in {"flight_delayed", "flight_delay"}:
            if "new_departure" in payload: flight["estimated_departure"] = payload["new_departure"]
            if "delay_minutes" in payload: flight["delay_minutes"] = payload["delay_minutes"]
        flight["status_classification"] = "OBSERVED"
        flight["stand_classification"] = "OBSERVED" if kind in {"stand_arrival", "gate_changed"} else flight["stand_classification"]
        flight["history"].append(observed)
    else:
        observed["entity_id"] = entity
        state["recent_events"].append(observed)
    state["observed_at"] = event["timestamp"]
    state["recent_events"].append({**observed, "entity_id": entity})
    state["recent_events"] = state["recent_events"][-12:]


def replay_timeline(dataset: dict[str, Any], events: list[dict[str, Any]]) -> dict[str, Any]:
    ordered = sorted(events, key=lambda event: (event["timestamp"], event["event_id"]))
    state = _initial_state(dataset)
    transitions = []
    for index, event in enumerate(ordered):
        _apply_event(state, event)
        transitions.append({"index": index, "event": event["event_id"], "timestamp": event["timestamp"],
                            "state": deepcopy(state)})
    normalized = {"dataset_id": dataset.get("dataset_id"),
                  "events": [_event_identity(event) for event in ordered],
                  "transitions": [{"event": item["event"], "timestamp": item["timestamp"], "state": item["state"]} for item in transitions]}
    return {"schema_version": SCHEMA_VERSION, "events": ordered, "transitions": transitions,
            "normalized_sha256": sha256(canonical_bytes(normalized)),
            "initial_state": _initial_state(dataset), "final_state": state}


class ReplayClock:
    """Virtual-time replay controls; callers advance simulation time explicitly."""
    def __init__(self, dataset: dict[str, Any], replay: dict[str, Any]):
        self.dataset = dataset
        self.replay = replay
        self.events = replay["events"]
        self.speed = 1
        self.playing = False
        self.index = -1
        self.playhead: str | None = None
        self.state = _initial_state(dataset)

    def snapshot(self) -> dict[str, Any]:
        return {"schema_version": SCHEMA_VERSION, "playing": self.playing, "speed": self.speed,
                "index": self.index, "event_count": len(self.events), "timestamp": self.playhead,
                "state": deepcopy(self.state), "replay_sha256": self.replay["normalized_sha256"]}

    def pause(self) -> dict[str, Any]:
        self.playing = False
        return self.snapshot()

    def play(self, speed: int | None = None) -> dict[str, Any]:
        if speed is not None:
            if speed not in SPEEDS: raise ReplayError("Replay speed must be 1x, 5x, 10x, or 60x.")
            self.speed = speed
        self.playing = self.index < len(self.events) - 1
        return self.snapshot()

    def step(self) -> dict[str, Any]:
        self.playing = False
        return self._apply_through(min(self.index + 1, len(self.events) - 1))

    def seek(self, timestamp: str) -> dict[str, Any]:
        target = iso_utc(_timestamp(timestamp))
        index = -1
        for i, event in enumerate(self.events):
            if event["timestamp"] <= target: index = i
            else: break
        self.playing = False
        self.state = _initial_state(self.dataset)
        self.index = -1
        self.playhead = target
        return self._apply_through(index, preserve_playhead=True)

    def advance(self, seconds: int | float) -> dict[str, Any]:
        if isinstance(seconds, bool) or not isinstance(seconds, (int, float)) or seconds < 0:
            raise ReplayError("Virtual replay advance must be a nonnegative number of seconds.")
        if self.playhead is None:
            if self.events: self.playhead = self.events[0]["timestamp"]
            else: return self.snapshot()
        target = _timestamp(self.playhead).timestamp() + seconds * self.speed
        playing = self.playing
        self.seek(datetime.fromtimestamp(target, timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z"))
        self.playing = playing and self.index < len(self.events) - 1
        return self.snapshot()

    def _apply_through(self, target_index: int, preserve_playhead: bool = False) -> dict[str, Any]:
        start = 0 if target_index < self.index else self.index + 1
        if target_index < self.index:
            self.state = _initial_state(self.dataset)
            start = 0
        for event in self.events[start:target_index + 1]:
            _apply_event(self.state, event)
        self.index = target_index
        if not preserve_playhead:
            self.playhead = self.events[target_index]["timestamp"] if target_index >= 0 else None
        return self.snapshot()


def freshness(received_events: list[dict[str, Any]], *, now: str | None = None,
              delayed_after_seconds: int = 300, stale_after_seconds: int = 900) -> dict[str, Any]:
    if delayed_after_seconds < 0 or stale_after_seconds < delayed_after_seconds:
        raise ReplayError("Freshness thresholds must be nonnegative and stale must be at least delayed.")
    if not received_events:
        return {"status": "UNKNOWN", "last_received_at": None, "age_seconds": None,
                "message": "No live updates have been received; no real-time feed is connected."}
    latest = max(received_events, key=lambda event: event.get("received_at", event["timestamp"]))
    received = _timestamp(latest.get("received_at", latest["timestamp"]))
    current = _timestamp(now or utc_now())
    if received > current:
        return {"status": "FUTURE", "last_received_at": iso_utc(received), "age_seconds": 0,
                "message": "The latest event is future-dated; it is not evidence of current operational freshness."}
    age = max(0, int((current - received).total_seconds()))
    status = "CURRENT" if age <= delayed_after_seconds else "DELAYED" if age <= stale_after_seconds else "STALE"
    return {"status": status, "last_received_at": iso_utc(received), "age_seconds": age,
            "message": "Live-update capable local session; this status reflects client submissions, not an airport feed."}


def plan_actual_from_events(dataset: dict[str, Any], events: list[dict[str, Any]]) -> list[dict[str, Any]]:
    event_map: dict[str, dict[str, Any]] = {}
    for event in sorted(events, key=lambda item: (item["timestamp"], item["event_id"])):
        if event["event_type"] not in {"vehicle_status", "closure_started", "closure_ended"}:
            event_map.setdefault(event["entity_id"], {})
            if event["event_type"] == "actual_arrival": event_map[event["entity_id"]]["actual_arrival"] = event["timestamp"]
            elif event["event_type"] == "actual_departure": event_map[event["entity_id"]]["actual_departure"] = event["timestamp"]
            elif event["event_type"] in {"stand_arrival", "gate_changed"}: event_map[event["entity_id"]]["actual_stand"] = event["payload"]["stand_id"]
    output = []
    for flight in dataset.get("flights", []):
        actual = event_map.get(flight["flight_id"], {})
        item = {"flight_id": flight["flight_id"], "scheduled_stand": flight.get("stand_id"),
                "actual_stand": actual.get("actual_stand"), "stand_variance": None if actual.get("actual_stand") is None else actual["actual_stand"] != flight.get("stand_id"),
                "evidence": "OBSERVED" if actual else "SCHEDULED"}
        for label, sched_key, actual_key in (("arrival", "scheduled_arrival", "actual_arrival"), ("departure", "scheduled_departure", "actual_departure")):
            planned = flight.get(sched_key)
            observed = flight.get(actual_key) or actual.get(actual_key)
            item[f"scheduled_{label}"] = planned
            item[f"actual_{label}"] = observed
            item[f"{label}_variance_minutes"] = None if not planned or not observed else round((_timestamp(observed) - _timestamp(planned)).total_seconds() / 60, 2)
        output.append(item)
    return sorted(output, key=lambda item: item["flight_id"])
