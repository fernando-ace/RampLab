"""Schedule import, mapping profiles, quality checks and Goal 24A adaptation."""
from __future__ import annotations

import csv
import hashlib
import io
import json
import re
from copy import deepcopy
from datetime import datetime, timedelta
from pathlib import Path
import tempfile
from typing import Any

try:
    from .operational_time import OperationalTimeError, is_time_only, iso_utc, local_clock, parse_date, parse_utc, zone
except ImportError:  # Support the dashboard's existing top-level script/test imports.
    from operational_time import OperationalTimeError, is_time_only, iso_utc, local_clock, parse_date, parse_utc, zone

SCHEMA_VERSION = "1.0"
GENERATION_VERSION = "1.0"
TARGETS = {
    "flight.id", "aircraft.id", "aircraft.type", "aircraft.tail_number",
    "flight.scheduled_arrival", "flight.scheduled_departure", "flight.actual_arrival",
    "flight.actual_departure", "flight.stand_id", "flight.actual_stand_id", "flight.status", "flight.origin", "flight.destination",
}
ID_TARGETS = {"flight.id", "aircraft.id", "aircraft.tail_number", "flight.stand_id"}
SUGGESTIONS = {
    "flight.id": ("flight", "flight no", "flight number", "flight_id", "callsign"),
    "aircraft.id": ("aircraft id", "tail", "tail number", "registration", "aircraft_id"),
    "aircraft.type": ("aircraft", "aircraft type", "type", "icao type"),
    "aircraft.tail_number": ("tail", "tail number", "registration", "tail_number"),
    "flight.scheduled_arrival": ("scheduled arrival", "eta", "sta", "arrival", "scheduled_arrival"),
    "flight.scheduled_departure": ("scheduled departure", "etd", "std", "departure", "scheduled_departure"),
    "flight.actual_arrival": ("actual arrival", "ata", "actual_arrival"),
    "flight.actual_departure": ("actual departure", "atd", "actual_departure"),
    "flight.stand_id": ("stand", "gate", "parking", "gate/stand", "stand_id"),
    "flight.actual_stand_id": ("actual stand", "actual gate", "actual_stand_id"),
    "flight.status": ("flight status", "status"),
    "flight.origin": ("origin", "from"),
    "flight.destination": ("destination", "to"),
}


class ScheduleImportError(ValueError):
    pass


def canonical_bytes(value: Any) -> bytes:
    return (json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"), allow_nan=False) + "\n").encode("utf-8")


def sha256(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def _header_key(value: str) -> str:
    return re.sub(r"[^a-z0-9]+", " ", value.casefold()).strip()


def parse_source(filename: str, content: str | bytes) -> tuple[list[dict[str, Any]], bytes, str]:
    raw = content if isinstance(content, bytes) else content.encode("utf-8")
    suffix = Path(filename).suffix.casefold()
    try:
        text = raw.decode("utf-8-sig")
        if suffix == ".csv":
            reader = csv.DictReader(io.StringIO(text, newline=""), strict=True)
            if not reader.fieldnames or len(reader.fieldnames) != len(set(reader.fieldnames)):
                raise ValueError("CSV requires unique column headers.")
            rows = list(reader)
            if any(None in row for row in rows):
                raise ValueError("A CSV row contains more values than its header.")
        elif suffix == ".json":
            parsed = json.loads(text, parse_constant=lambda item: (_ for _ in ()).throw(ValueError(f"Invalid JSON number {item}.")))
            rows = parsed.get("records", parsed) if isinstance(parsed, dict) else parsed
            if not isinstance(rows, list) or any(not isinstance(row, dict) for row in rows):
                raise ValueError("JSON schedule must be an array of objects or an object with a records array.")
        else:
            raise ValueError("Schedule import accepts CSV and JSON files.")
    except (UnicodeError, csv.Error, json.JSONDecodeError, ValueError) as exc:
        raise ScheduleImportError(f"{Path(filename).name}: {exc}") from None
    headers = list(rows[0]) if rows else []
    return rows, raw, suffix[1:]


def suggest_mapping(headers: list[str]) -> dict[str, str]:
    by_normalized = {_header_key(name): name for name in headers}
    suggestions: dict[str, str] = {}
    for target, aliases in SUGGESTIONS.items():
        for alias in aliases:
            if _header_key(alias) in by_normalized:
                suggestions[target] = by_normalized[_header_key(alias)]
                break
    return suggestions


def preview(filename: str, content: str | bytes) -> dict[str, Any]:
    rows, raw, kind = parse_source(filename, content)
    headers = list(rows[0]) if rows else []
    return {"filename": Path(filename).name, "format": kind, "headers": headers,
            "row_count": len(rows), "preview_rows": rows[:8], "suggested_fields": suggest_mapping(headers),
            "source_sha256": sha256(raw)}


def validate_profile(profile: dict[str, Any], headers: list[str] | None = None) -> dict[str, Any]:
    if not isinstance(profile, dict):
        raise ScheduleImportError("Mapping profile must be an object.")
    if profile.get("schema_version") != SCHEMA_VERSION:
        raise ScheduleImportError("Mapping profile schema_version must be 1.0.")
    if not isinstance(profile.get("airport_id"), str) or not profile["airport_id"].strip():
        raise ScheduleImportError("Mapping profile needs an airport_id.")
    if not isinstance(profile.get("timezone"), str) or not profile["timezone"].strip():
        raise ScheduleImportError("Mapping profile needs an IANA airport timezone.")
    zone(profile["timezone"])
    if not profile.get("operational_date"):
        raise ScheduleImportError("Mapping profile needs the airport-local operational date.")
    fields = profile.get("fields", {})
    if not isinstance(fields, dict):
        raise ScheduleImportError("Mapping profile fields must map canonical targets to source columns.")
    unknown = sorted(set(fields) - TARGETS)
    if unknown:
        raise ScheduleImportError("Unsupported mapping target(s): " + ", ".join(unknown))
    if headers is not None:
        required_targets = {"flight.id", "aircraft.id"}
        if not any(fields.get(target) or target in profile.get("defaults", {}) for target in ("flight.scheduled_arrival", "flight.scheduled_departure")):
            raise ScheduleImportError("Map at least one scheduled arrival or departure field.")
        required_sources = {fields[target] for target in required_targets if fields.get(target) and target not in profile.get("defaults", {})}
        missing = sorted(source for source in required_sources if source not in headers)
        if missing:
            raise ScheduleImportError("Required mapped source column(s) are missing: " + ", ".join(missing))
    defaults = profile.get("defaults", {})
    if not isinstance(defaults, dict) or set(defaults) - TARGETS:
        raise ScheduleImportError("Profile defaults must use supported canonical targets.")
    value_maps = profile.get("value_maps", {})
    if not isinstance(value_maps, dict) or set(value_maps) - {"flight.stand_id", "flight.actual_stand_id", "aircraft.type", "flight.status"}:
        raise ScheduleImportError("Profile value_maps may map stand IDs, aircraft types, or flight statuses.")
    for key, mapping in value_maps.items():
        if not isinstance(mapping, dict) or any(not isinstance(k, str) or not isinstance(v, str) or not v.strip() for k, v in mapping.items()):
            raise ScheduleImportError(f"The {key} value map must contain non-empty string mappings.")
    scenario_defaults = profile.get("scenario_defaults", {})
    if not isinstance(scenario_defaults, dict) or set(scenario_defaults) - {"turnaround_services"}:
        raise ScheduleImportError("scenario_defaults may define only turnaround_services.")
    services = scenario_defaults.get("turnaround_services", [])
    if not isinstance(services, list):
        raise ScheduleImportError("scenario_defaults.turnaround_services must be a list.")
    for index, item in enumerate(services):
        if not isinstance(item, dict) or not isinstance(item.get("service_type"), str) or not item["service_type"].strip():
            raise ScheduleImportError(f"Turnaround service assumption {index + 1} needs a service_type.")
        duration = item.get("duration_minutes")
        if isinstance(duration, bool) or not isinstance(duration, (int, float)) or duration <= 0:
            raise ScheduleImportError(f"Turnaround service assumption {index + 1} needs a positive duration_minutes value.")
    date_value = profile.get("operational_date")
    if date_value:
        parse_date(date_value)
    if not isinstance(profile.get("profile_id", ""), str) or not isinstance(profile.get("name", ""), str):
        raise ScheduleImportError("Mapping profile id and name must be strings.")
    return profile


def _source_value(row: dict[str, Any], profile: dict[str, Any], target: str) -> tuple[Any, str]:
    source = profile.get("fields", {}).get(target)
    if source:
        value = row.get(source)
        if value is not None and str(value).strip() != "":
            return value, "source"
    if target in profile.get("defaults", {}):
        return profile["defaults"][target], "configured_default"
    return None, "unknown"


def _identifier(value: Any, policy: str | None) -> str | None:
    if value is None or not str(value).strip():
        return None
    result = str(value).strip()
    if policy == "uppercase": return result.upper()
    if policy == "casefold": return result.casefold()
    return result


def _timestamp(value: Any, profile: dict[str, Any], target: str, row: int,
               findings: list[dict[str, Any]]) -> str | None:
    if value is None or not str(value).strip(): return None
    try:
        parsed = parse_utc(value, profile["timezone"], timestamp_format=profile.get("timestamp_format"),
                           operational_date=profile.get("operational_date"), fold=profile.get("dst_fold"))
        return iso_utc(parsed)
    except OperationalTimeError as exc:
        findings.append(_finding("TIMESTAMP_INVALID", "ERROR", str(exc), row, target, value))
        return None


def _finding(code: str, severity: str, message: str, row: int | None = None,
             field: str | None = None, value: Any = None, source: str = "") -> dict[str, Any]:
    return {"severity": severity, "code": code, "message": message,
            "source": source, "row": row, "field": field, "value": value}


def _time_only_sort(value: Any, profile: dict[str, Any]) -> datetime | None:
    if not isinstance(value, str) or not is_time_only(value, profile.get("timestamp_format")):
        return None
    try:
        return datetime.strptime(value.strip(), profile.get("timestamp_format") or ("%H:%M:%S" if value.count(":") == 2 else "%H:%M"))
    except ValueError:
        return None


def _records_to_operational(rows: list[dict[str, Any]], profile: dict[str, Any], filename: str,
                            source_hash: str) -> dict[str, Any]:
    validate_profile(profile, list(rows[0]) if rows else [])
    operational_date = parse_date(profile.get("operational_date")) if profile.get("operational_date") else None
    fields = profile.get("fields", {})
    findings: list[dict[str, Any]] = []
    flights: list[dict[str, Any]] = []
    seen_flights: dict[str, int] = {}
    aircraft: dict[str, dict[str, Any]] = {}
    for index, source_row in enumerate(rows, start=2 if Path(filename).suffix.casefold() == ".csv" else 1):
        values: dict[str, Any] = {}
        sources: dict[str, str] = {}
        for target in TARGETS:
            values[target], sources[target] = _source_value(source_row, profile, target)
            policy = profile.get("identifier_normalization", {}).get(target)
            if target in ID_TARGETS or target == "flight.actual_stand_id":
                values[target] = _identifier(values[target], policy)
        flight_id, aircraft_id = values["flight.id"], values["aircraft.id"]
        if not flight_id:
            findings.append(_finding("FLIGHT_ID_MISSING", "ERROR", "Flight identifier is required.", index, "flight.id", None, filename))
            continue
        if flight_id in seen_flights:
            findings.append(_finding("FLIGHT_ID_DUPLICATE", "ERROR", f"Duplicate flight identifier {flight_id!r} (first seen on row {seen_flights[flight_id]}).", index, "flight.id", flight_id, filename))
        else:
            seen_flights[flight_id] = index
        if not aircraft_id:
            findings.append(_finding("AIRCRAFT_ID_MISSING", "ERROR", "Aircraft identifier is required; no value was inferred.", index, "aircraft.id", None, filename))
            continue
        schedule = {key: values[key] for key in ("flight.scheduled_arrival", "flight.scheduled_departure")}
        actual = {key: values[key] for key in ("flight.actual_arrival", "flight.actual_departure")}
        if not any(schedule.values()):
            findings.append(_finding("SCHEDULE_TIME_MISSING", "ERROR", "At least one scheduled arrival or departure time is required.", index, "flight.scheduled_arrival", None, filename))
        if not values["aircraft.type"]:
            findings.append(_finding("AIRCRAFT_TYPE_MISSING", "WARNING", "Aircraft type is unknown; schedule is retained but scenario generation may need an explicit configured assumption.", index, "aircraft.type", None, filename))
        else:
            allowed_types = profile.get("allowed_aircraft_types")
            normalized_type = str(values["aircraft.type"]).strip()
            type_map = profile.get("value_maps", {}).get("aircraft.type", {})
            normalized_type = type_map.get(normalized_type, normalized_type)
            values["aircraft.type"] = normalized_type
            if allowed_types is not None and normalized_type not in allowed_types:
                findings.append(_finding("AIRCRAFT_TYPE_UNSUPPORTED", "WARNING", f"Aircraft type {normalized_type!r} is not listed in this profile's supported type set.", index, "aircraft.type", normalized_type, filename))
        for stand_target in ("flight.stand_id", "flight.actual_stand_id"):
            stand_value = values[stand_target]
            if stand_value:
                stand_map = profile.get("value_maps", {}).get(stand_target, profile.get("value_maps", {}).get("flight.stand_id", {}))
                if stand_value not in stand_map:
                    findings.append(_finding("STAND_UNKNOWN", "ERROR", f"Stand {stand_value!r} has no explicit profile mapping.", index, stand_target, stand_value, filename))
                else:
                    values[stand_target] = stand_map[stand_value]
        if not values["flight.stand_id"]:
            findings.append(_finding("STAND_MISSING", "WARNING", "Stand assignment is unknown; no reassignment was inferred.", index, "flight.stand_id", None, filename))
        sched_arr_raw, sched_dep_raw = schedule.values()
        arr_clock = _time_only_sort(sched_arr_raw, profile)
        dep_clock = _time_only_sort(sched_dep_raw, profile)
        rolled_departure = False
        if sched_arr_raw and sched_dep_raw and arr_clock and dep_clock and dep_clock < arr_clock:
            rolled_departure = True
        times = {}
        for target, raw in {**schedule, **actual}.items():
            parse_profile = profile
            if target == "flight.scheduled_departure" and rolled_departure and profile.get("timestamp_format") and operational_date:
                from datetime import date
                parse_profile = {**profile, "operational_date": (date.fromisoformat(operational_date) + timedelta(days=1)).isoformat()}
            elif target == "flight.scheduled_departure" and rolled_departure and operational_date and not profile.get("timestamp_format"):
                from datetime import date
                parse_profile = {**profile, "operational_date": (date.fromisoformat(operational_date) + timedelta(days=1)).isoformat()}
            times[target] = _timestamp(raw, parse_profile, target, index, findings)
        arr, dep = times["flight.scheduled_arrival"], times["flight.scheduled_departure"]
        if arr and dep and dep < arr:
            findings.append(_finding("DEPARTURE_BEFORE_ARRIVAL", "ERROR", "Scheduled departure is before scheduled arrival after timezone normalization.", index, "flight.scheduled_departure", sched_dep_raw, filename))
        if arr and dep and dep == arr:
            findings.append(_finding("ZERO_TURNAROUND_WINDOW", "WARNING", "Scheduled arrival and departure are the same instant.", index, "flight.scheduled_departure", sched_dep_raw, filename))
        status = values["flight.status"]
        status_map = profile.get("value_maps", {}).get("flight.status", {})
        status = status_map.get(str(status), status) if status is not None else None
        aircraft.setdefault(aircraft_id, {"aircraft_id": aircraft_id,
                                           "type": values["aircraft.type"] or profile.get("defaults", {}).get("aircraft.type"),
                                           "registration": values["aircraft.tail_number"] or aircraft_id,
                                           "model": values["aircraft.type"] or None,
                                           "type_source": sources.get("aircraft.type", "unknown"),
                                           "source": {"filename": filename, "row": index}})
        source_fields = {target: {"source_column": fields.get(target), "raw_value": source_row.get(fields.get(target)) if fields.get(target) else None,
                                  "classification": sources[target]} for target in sorted(TARGETS) if values.get(target) is not None}
        provenance = {"source": filename, "row": index, "source_sha256": source_hash,
                      "mapping_profile_id": profile.get("profile_id", ""), "fields": source_fields}
        flights.append({"flight_id": flight_id, "aircraft_id": aircraft_id, "aircraft_type": values["aircraft.type"],
                        "tail_number": values["aircraft.tail_number"], "scheduled_arrival": arr,
                        "scheduled_departure": dep, "actual_arrival": times["flight.actual_arrival"],
                        "actual_departure": times["flight.actual_departure"], "stand_id": values["flight.stand_id"],
                        "actual_stand_id": values["flight.actual_stand_id"],
                        "status": status, "origin": values["flight.origin"], "destination": values["flight.destination"],
                        "quality": "ERROR" if any(f.get("row") == index and f["severity"] == "ERROR" for f in findings) else "WARNING" if any(f.get("row") == index and f["severity"] == "WARNING" for f in findings) else "VALID",
                        "source": provenance, "rollover_applied": rolled_departure})
    # Stand occupancy uses only supplied arrival/departure windows. Unknown edges
    # remain unknown instead of receiving an assumed duration.
    by_stand: dict[str, list[dict[str, Any]]] = {}
    for flight in flights:
        if flight.get("stand_id") and flight.get("scheduled_arrival") and flight.get("scheduled_departure"):
            by_stand.setdefault(flight["stand_id"], []).append(flight)
    for stand, assigned in by_stand.items():
        for i, left in enumerate(assigned):
            for right in assigned[i + 1:]:
                if left["scheduled_arrival"] < right["scheduled_departure"] and right["scheduled_arrival"] < left["scheduled_departure"]:
                    findings.append(_finding("STAND_OCCUPANCY_CONFLICT", "ERROR", f"{left['flight_id']} and {right['flight_id']} overlap on stand {stand}.",
                                             right["source"]["row"], "flight.stand_id", stand, filename))
    findings.sort(key=lambda item: (item["severity"], item["code"], item.get("source", ""), item.get("row") or 0, item.get("field") or ""))
    normalized = {"schema_version": SCHEMA_VERSION, "dataset_id": "", "airport_id": profile["airport_id"].strip().upper(),
                  "operational_date": operational_date, "timezone": profile["timezone"], "flights": sorted(flights, key=lambda item: (item.get("scheduled_arrival") or item.get("scheduled_departure") or "", item["flight_id"])),
                  "aircraft": [aircraft[key] for key in sorted(aircraft)], "events": [],
                  "provenance": {"source_filename": Path(filename).name, "source_format": Path(filename).suffix.lower().lstrip("."),
                                 "source_sha256": source_hash, "mapping_profile": deepcopy(profile),
                                 "mapping_profile_sha256": sha256(canonical_bytes(profile)), "ingestion_version": SCHEMA_VERSION,
                                 "generation_version": GENERATION_VERSION},
                  "findings": findings, "quality_summary": {"errors": sum(f["severity"] == "ERROR" for f in findings),
                                     "warnings": sum(f["severity"] == "WARNING" for f in findings),
                                     "info": sum(f["severity"] == "INFO" for f in findings)}}
    identity_view = {k: v for k, v in normalized.items() if k not in {"dataset_id", "provenance", "findings", "quality_summary"}}
    identity_view["provenance"] = {k: v for k, v in normalized["provenance"].items() if k != "ingested_at"}
    normalized["normalized_sha256"] = sha256(canonical_bytes(identity_view))
    normalized["dataset_id"] = "ops_" + normalized["normalized_sha256"][:20]
    normalized["provenance"]["ingested_at"] = datetime.now().astimezone().isoformat(timespec="seconds")
    normalized["quality_summary"]["status"] = "ERROR" if normalized["quality_summary"]["errors"] else "VALID_WITH_WARNINGS" if normalized["quality_summary"]["warnings"] else "VALID"
    return normalized


def import_schedule(filename: str, content: str | bytes, profile: dict[str, Any]) -> dict[str, Any]:
    rows, raw, kind = parse_source(filename, content)
    if not rows:
        raise ScheduleImportError("Schedule file contains no records.")
    validate_profile(profile, list(rows[0]))
    if profile.get("source_format") and profile["source_format"] != kind:
        raise ScheduleImportError(f"Mapping profile expects {profile['source_format']} but source is {kind}.")
    result = _records_to_operational(rows, profile, Path(filename).name, sha256(raw))
    result["provenance"]["source_format"] = kind
    result["provenance"]["operational_date"] = result["operational_date"]
    if profile.get("data_classification"):
        result["provenance"]["data_classification"] = profile["data_classification"]
    return result


def to_canonical_package(dataset: dict[str, Any]) -> dict[str, Any]:
    """Adapt a validated operational schedule to Goal 24A's canonical model."""
    if dataset.get("quality_summary", {}).get("errors"):
        first = next((item for item in dataset.get("findings", []) if item.get("severity") == "ERROR"), None)
        detail = f" {first['message']}" if first else ""
        raise ScheduleImportError("Resolve schedule data-quality errors before generating a RampLab scenario." + detail)
    if not dataset.get("flights"):
        raise ScheduleImportError("Schedule has no flights.")
    stands = sorted({flight["stand_id"] for flight in dataset["flights"] if flight.get("stand_id")})
    if not stands:
        raise ScheduleImportError("A simulator-compatible stand mapping is required before scenario generation.")
    aircraft_by_id = {row["aircraft_id"]: row for row in dataset["aircraft"]}
    service_defaults = dataset.get("provenance", {}).get("mapping_profile", {}).get("scenario_defaults", {}).get("turnaround_services", [])
    turnaround_requirements = []
    for aircraft_id in sorted(aircraft_by_id):
        safe_aircraft_id = re.sub(r"[^A-Za-z0-9_-]", "_", aircraft_id)
        for index, service in enumerate(service_defaults, start=1):
            turnaround_requirements.append({"requirement_id": f"OPS-{safe_aircraft_id}-{index}",
                                            "aircraft_id": aircraft_id,
                                            "service_type": service["service_type"],
                                            "duration_minutes": float(service["duration_minutes"])})
    canonical_flights, occupancy_by_stand = [], {}
    for row in dataset["flights"]:
        if not row.get("aircraft_type"):
            raise ScheduleImportError(f"Flight {row['flight_id']} has no aircraft type. Configure an explicit profile default before scenario generation.")
        if not row.get("stand_id"):
            raise ScheduleImportError(f"Flight {row['flight_id']} has no mapped stand. Assign one explicitly before scenario generation.")
        legs = []
        if row.get("scheduled_arrival"):
            legs.append(("arrival", row["scheduled_arrival"], row.get("actual_arrival"), "ARR"))
        if row.get("scheduled_departure"):
            legs.append(("departure", row["scheduled_departure"], row.get("actual_departure"), "DEP"))
        for operation, scheduled, actual, suffix in legs:
            canonical_id = f"{row['flight_id']}::{suffix}"
            flight = {"flight_id": canonical_id, "flight_number": row["flight_id"], "operation": operation,
                      "scheduled_time": scheduled, "aircraft_id": row["aircraft_id"], "gate_id": row["stand_id"],
                      "operational_dataset_id": dataset["dataset_id"]}
            if operation == "arrival" and row.get("origin"):
                flight["origin"] = row["origin"]
            if operation == "departure" and row.get("destination"):
                flight["destination"] = row["destination"]
            canonical_flights.append(flight)
        if row.get("scheduled_arrival") and row.get("scheduled_departure"):
            occupancy_by_stand.setdefault(row["stand_id"], []).append((row["scheduled_arrival"], row["scheduled_departure"], row["flight_id"]))
    for stand, intervals in occupancy_by_stand.items():
        for index, left in enumerate(intervals):
            for right in intervals[index + 1:]:
                if left[0] < right[1] and right[0] < left[1]:
                    raise ScheduleImportError(f"Stand {stand} has overlapping schedule assignments for {left[2]} and {right[2]}.")
    return {"schema_version": "1.0", "airport": {"airport_id": dataset["airport_id"], "timezone": "UTC",
                                                       "operational_timezone": dataset["timezone"]},
            "dataset": {"dataset_id": dataset["dataset_id"], "description": "Imported operational schedule; source and profile provenance retained."},
            "sources": [{"filename": dataset["provenance"]["source_filename"], "type": dataset["provenance"]["source_format"],
                         "sha256": dataset["provenance"]["source_sha256"], "record_count": len(dataset["flights"]),
                         "accepted_count": len(dataset["flights"]), "rejected_count": 0}],
            "flights": sorted(canonical_flights, key=lambda row: (row["scheduled_time"], row["flight_id"])),
            "aircraft": [{"aircraft_id": item["aircraft_id"], "type": item["type"],
                          **({"registration": item["registration"]} if item.get("registration") else {})}
                         for item in aircraft_by_id.values() if item.get("type")],
            "gates": [{"gate_id": stand, "status": "available"} for stand in stands],
            "equipment": [], "turnaround_requirements": turnaround_requirements, "disruptions": [],
            "validation": {"status": "valid", "counts": {"flights": len(canonical_flights), "aircraft": len(aircraft_by_id), "gates": len(stands)}, "findings": []},
            "operational_provenance": {"schema_version": SCHEMA_VERSION, "dataset_id": dataset["dataset_id"],
                                       "normalized_sha256": dataset["normalized_sha256"],
                                       "mapping_profile_sha256": dataset["provenance"]["mapping_profile_sha256"],
                                       "source_sha256": dataset["provenance"]["source_sha256"],
                                       "source_filename": dataset["provenance"]["source_filename"],
                                       "source_format": dataset["provenance"]["source_format"],
                                       "record_count": len(dataset["flights"]),
                                       "timezone": dataset["timezone"], "operational_date": dataset["operational_date"]}}


def validate_through_goal24a(dataset: dict[str, Any]) -> dict[str, Any]:
    """Run the canonical package through the established Goal 24A validator."""
    package = to_canonical_package(dataset)
    tables = {name: package[name] for name in ("flights", "aircraft", "gates", "equipment", "turnaround_requirements", "disruptions")}
    with tempfile.TemporaryDirectory(prefix="ramplab-operational-goal24a-") as temporary:
        root = Path(temporary)
        files = {}
        for name, rows in tables.items():
            path = root / f"{name}.json"
            path.write_bytes(canonical_bytes(rows))
            files[name] = path.name
        manifest = {"schema_version": "1.0", "dataset_id": dataset["dataset_id"],
                    "description": "Operational schedule normalized through Goal 24A; see operational_provenance.",
                    "airport": {"airport_id": dataset["airport_id"], "timezone": "UTC"}, "files": files}
        manifest_path = root / "manifest.json"
        manifest_path.write_bytes(canonical_bytes(manifest))
        from tools.airport_data_ingestion.core import load_dataset
        normalized, findings, _counts = load_dataset(manifest_path)
    if normalized is None or normalized.get("validation", {}).get("status") != "valid":
        errors = [item.get("message", "Goal 24A validation failed") for item in findings if item.get("severity") == "error"]
        raise ScheduleImportError("Goal 24A rejected the imported schedule: " + "; ".join(errors[:5]))
    normalized["airport"]["operational_timezone"] = dataset["timezone"]
    normalized["schema_version"] = "1.0"
    normalized["operational_provenance"] = deepcopy(package["operational_provenance"])
    normalized["dataset"]["description"] = "Imported operational schedule. Source and mapping identity are recorded in operational_provenance."
    normalized["dataset"]["operational_date"] = dataset["operational_date"]
    return normalized


def plan_vs_actual(dataset: dict[str, Any], events: list[dict[str, Any]] | None = None) -> list[dict[str, Any]]:
    event_actuals: dict[str, dict[str, str]] = {}
    for event in sorted(events or [], key=lambda row: (row["timestamp"], row["event_id"])):
        actuals = event_actuals.setdefault(event["entity_id"], {})
        if event["event_type"] in {"actual_arrival", "actual_departure"}:
            actuals[event["event_type"]] = event["timestamp"]
        elif event["event_type"] in {"stand_arrival", "gate_changed"}:
            actuals["actual_stand_id"] = event["payload"]["stand_id"]
    report = []
    for flight in dataset.get("flights", []):
        actuals = event_actuals.get(flight["flight_id"], {})
        stand_actual = flight.get("actual_stand_id") or actuals.get("actual_stand_id")
        has_observed = bool(stand_actual or flight.get("actual_arrival") or flight.get("actual_departure") or actuals)
        row = {"flight_id": flight["flight_id"], "stand_scheduled": flight.get("stand_id"), "stand_actual": stand_actual,
               "stand_variance": None if stand_actual is None else stand_actual != flight.get("stand_id"),
               "evidence": "OBSERVED" if has_observed else "SCHEDULED"}
        for label, scheduled, actual in (("arrival", "scheduled_arrival", "actual_arrival"),
                                         ("departure", "scheduled_departure", "actual_departure")):
            planned = flight.get(scheduled)
            event_key = "actual_arrival" if label == "arrival" else "actual_departure"
            observed = flight.get(actual) or actuals.get(event_key)
            row[f"scheduled_{label}"] = planned
            row[f"actual_{label}"] = observed
            if planned and observed:
                row[f"{label}_variance_minutes"] = round((datetime.fromisoformat(observed.replace("Z", "+00:00")) -
                                                          datetime.fromisoformat(planned.replace("Z", "+00:00"))).total_seconds() / 60, 2)
                row["evidence"] = "OBSERVED"
            else:
                row[f"{label}_variance_minutes"] = None
        report.append(row)
    return sorted(report, key=lambda row: row["flight_id"])
