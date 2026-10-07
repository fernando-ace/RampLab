"""Parsing, validation, normalization and canonical serialization."""
from __future__ import annotations

import csv
import hashlib
import io
import json
import math
import re
from datetime import datetime, timezone
from pathlib import Path
from zoneinfo import ZoneInfo, ZoneInfoNotFoundError

VERSION = "1.0"
TABLES = ("flights", "aircraft", "gates", "equipment", "turnaround_requirements", "disruptions")
REQUIRED = {
    "flights": ("flight_id", "operation", "scheduled_time", "aircraft_id"),
    "aircraft": ("aircraft_id", "type"), "gates": ("gate_id",),
    "equipment": ("equipment_id", "type", "status"),
    "turnaround_requirements": ("requirement_id", "service_type", "duration_minutes"),
    "disruptions": ("disruption_id", "type", "start_time"),
}
ENUMS = {"operation": {"arrival", "departure"}, "status": {"available", "unavailable", "maintenance", "active", "inactive", "open", "closed", "out_of_service"},
         "type": {"tug", "baggage_vehicle", "fuel_vehicle", "catering_vehicle", "service_cart", "ground_service_vehicle"}}
ID_FIELDS = {"flights": "flight_id", "aircraft": "aircraft_id", "gates": "gate_id", "equipment": "equipment_id",
             "turnaround_requirements": "requirement_id", "disruptions": "disruption_id"}

class InputFailure(Exception):
    pass

def _reject_constant(value):
    raise ValueError(f"non-standard JSON numeric constant {value}")

def _finding(code, severity, message, source="", record=None, field="", value=None):
    return {"code": code, "severity": severity, "source": source, "record": record, "field": field,
            "value": value, "message": message}

def _parse(path: Path):
    raw = path.read_bytes()
    try:
        if path.suffix.lower() == ".json":
            data = json.loads(raw.decode("utf-8-sig"), parse_constant=_reject_constant)
            if isinstance(data, dict):
                data = data.get("records", data.get(path.stem, data))
            if not isinstance(data, list) or any(not isinstance(r, dict) for r in data):
                raise ValueError("expected a JSON array of objects")
            return raw, data
        if path.suffix.lower() == ".csv":
            text = raw.decode("utf-8-sig")
            reader = csv.DictReader(io.StringIO(text, newline=""), strict=True)
            if not reader.fieldnames or len(reader.fieldnames) != len(set(reader.fieldnames)):
                raise ValueError("CSV requires unique column headers")
            rows = list(reader)
            if any(None in row for row in rows):
                raise ValueError("CSV row has more fields than the header")
            return raw, rows
        raise ValueError("supported source formats are CSV and JSON")
    except (UnicodeError, csv.Error, json.JSONDecodeError, ValueError) as e:
        raise InputFailure(f"{path.name}: {e}") from None

def _timestamp(value, tz_name):
    if value is None or str(value).strip() == "":
        return None
    s = str(value).strip()
    try:
        dt = datetime.fromisoformat(s.replace("Z", "+00:00"))
    except ValueError:
        for fmt in ("%Y-%m-%d %H:%M", "%Y-%m-%d %H:%M:%S", "%Y/%m/%d %H:%M"):
            try:
                dt = datetime.strptime(s, fmt); break
            except ValueError:
                dt = None
        if dt is None:
            raise ValueError("expected ISO timestamp or YYYY-MM-DD HH:MM local time")
    if dt.tzinfo is None:
        try: zone = timezone.utc if tz_name in ("UTC", "Etc/UTC", "GMT") else ZoneInfo(tz_name)
        except (ZoneInfoNotFoundError, TypeError): raise ValueError(f"unknown or missing airport timezone {tz_name!r}; install the platform IANA timezone database")
        a, b = dt.replace(tzinfo=zone, fold=0), dt.replace(tzinfo=zone, fold=1)
        if a.utcoffset() != b.utcoffset():
            raise ValueError("ambiguous or nonexistent local time; provide an explicit UTC offset")
        dt = a
    return dt.astimezone(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")

def load_dataset(manifest_path: Path):
    findings, counts, sources, tables = [], {}, [], {name: [] for name in TABLES}
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8-sig"), parse_constant=_reject_constant)
        if not isinstance(manifest, dict): raise ValueError("manifest must be a JSON object")
    except (OSError, ValueError) as e:
        return None, [_finding("MANIFEST_INVALID", "error", str(e), manifest_path.name)], counts
    if manifest.get("schema_version") != VERSION:
        findings.append(_finding("SCHEMA_VERSION_UNSUPPORTED", "error", f"schema_version must be {VERSION}", manifest_path.name, "schema_version", value=manifest.get("schema_version")))
    airport = manifest.get("airport")
    if not isinstance(airport, dict) or not str(airport.get("airport_id", "")).strip() or not airport.get("timezone"):
        findings.append(_finding("MANIFEST_AIRPORT_REQUIRED", "error", "airport.airport_id and airport.timezone are required", manifest_path.name))
        airport = airport if isinstance(airport, dict) else {}
    try:
        zone_name = str(airport.get("timezone", ""))
        if zone_name not in ("UTC", "Etc/UTC", "GMT"): ZoneInfo(zone_name)
    except (ZoneInfoNotFoundError, ValueError): findings.append(_finding("TIMEZONE_INVALID", "error", "airport.timezone must be a valid IANA timezone", manifest_path.name, "airport.timezone"))
    files = manifest.get("files", {})
    if not isinstance(files, dict):
        findings.append(_finding("MANIFEST_FILES_INVALID", "error", "files must map table names to relative file paths", manifest_path.name)); files = {}
    base = manifest_path.parent
    for table in TABLES:
        rel = files.get(table)
        if not rel:
            if table in ("flights", "aircraft", "gates"):
                findings.append(_finding("MANIFEST_SOURCE_REQUIRED", "error", f"files.{table} is required", manifest_path.name))
            counts[table] = 0; continue
        p = Path(str(rel))
        if p.is_absolute() or ".." in p.parts:
            findings.append(_finding("MANIFEST_PATH_UNSAFE", "error", "source paths must be relative and stay inside the dataset directory", manifest_path.name, field=table, value=rel)); continue
        path = base / p
        try: raw, rows = _parse(path)
        except (OSError, InputFailure) as e:
            findings.append(_finding("SOURCE_PARSE_ERROR", "error", str(e), str(rel))); counts[table] = 0; continue
        counts[table] = len(rows)
        sources.append({"filename": p.as_posix(), "type": path.suffix.lower()[1:], "sha256": hashlib.sha256(raw).hexdigest(), "record_count": len(rows), "accepted_record_count": len(rows), "rejected_record_count": 0})
        seen = {}; accepted = 0
        for index, source_row in enumerate(rows, 1):
            original = source_row.copy()
            row = {str(k).strip(): (v.strip() if isinstance(v, str) else v) for k, v in source_row.items()}
            bad = False
            for field in REQUIRED[table]:
                if row.get(field) in (None, ""):
                    findings.append(_finding("REQUIRED_FIELD_MISSING", "error", f"required field {field} is missing", str(rel), index, field)); bad = True
            idfield = ID_FIELDS[table]; ident = row.get(idfield)
            if ident:
                normalized = str(ident).strip()
                if normalized in seen:
                    old_index, old_raw = seen[normalized]
                    code = "DUPLICATE_ID" if str(old_raw) == str(original.get(idfield)) else "NORMALIZED_ID_COLLISION"
                    findings.append(_finding(code, "error", f"identifier {normalized!r} conflicts with record {old_index} after trimming whitespace", str(rel), index, idfield, ident)); bad = True
                else: seen[normalized] = (index, original.get(idfield))
                row[idfield] = normalized
            for field, allowed in ENUMS.items():
                if field in row and row[field] not in (None, ""):
                    val = re.sub(r"[ -]+", "_", str(row[field]).strip().lower())
                    if field == "type" and table != "equipment": continue
                    if val not in allowed:
                        findings.append(_finding("ENUM_UNSUPPORTED", "error", f"unsupported {field} value {row[field]!r}", str(rel), index, field, row[field])); bad = True
                    else: row[field] = val
            for field in ("scheduled_time", "estimated_time", "actual_time", "start_time", "end_time", "earliest_start", "occupancy_start", "occupancy_end"):
                if row.get(field):
                    try: row[field] = _timestamp(row[field], airport.get("timezone"))
                    except ValueError as e: findings.append(_finding("TIMESTAMP_INVALID", "error", str(e), str(rel), index, field, row[field])); bad = True
            for field in ("duration_minutes", "delay_minutes", "capacity"):
                if row.get(field) not in (None, ""):
                    try:
                        n = float(row[field]);
                        if not math.isfinite(n): raise ValueError("must be finite")
                        if n < 0: raise ValueError("must be non-negative")
                        row[field] = int(n) if n.is_integer() else n
                    except (ValueError, TypeError): findings.append(_finding("DURATION_INVALID", "error", "must be a non-negative number of minutes", str(rel), index, field, row[field])); bad = True
            if table == "aircraft":
                for dimension in ("wingspan", "length"):
                    if row.get(dimension) not in (None, ""):
                        try:
                            value = float(row[dimension]); unit = str(row.get(dimension + "_unit", "m")).strip().lower()
                            if not math.isfinite(value): raise ValueError("dimension must be finite")
                            factors = {"m": 1.0, "meter": 1.0, "meters": 1.0, "ft": 0.3048, "feet": 0.3048, "in": 0.0254}
                            if unit not in factors: raise ValueError("unit must be m, ft, or in")
                            row[dimension] = value * factors[unit]; row[dimension + "_unit"] = "m"
                        except (TypeError, ValueError) as e:
                            findings.append(_finding("UNIT_INVALID", "error", str(e) or "dimension must be numeric", str(rel), index, dimension, row.get(dimension))); bad = True
            for field in ("remote", "available"):
                if field in row and isinstance(row[field], str):
                    value = row[field].strip().lower()
                    if value in ("true", "yes", "1"): row[field] = True
                    elif value in ("false", "no", "0"): row[field] = False
                    else: findings.append(_finding("BOOLEAN_INVALID", "error", "expected true/false, yes/no, or 1/0", str(rel), index, field, row[field])); bad = True
            row = {k: v for k, v in row.items() if v not in (None, "")}
            if not bad: tables[table].append(row); accepted += 1
        if sources and sources[-1]["filename"] == p.as_posix():
            sources[-1]["accepted_record_count"] = accepted
            sources[-1]["rejected_record_count"] = len(rows) - accepted
        if len(rows) == 0 and table in ("flights", "aircraft", "gates"):
            findings.append(_finding("DATASET_EMPTY", "error", f"required dataset {table} is empty", str(rel)))
    ids = {key: {r.get(field) for r in tables[key]} for key, field in ID_FIELDS.items()}
    for r in tables["flights"]:
        if r.get("aircraft_id") not in ids["aircraft"]: findings.append(_finding("FLIGHT_UNKNOWN_AIRCRAFT", "error", f"aircraft {r.get('aircraft_id')!r} does not exist", field="aircraft_id", value=r.get("aircraft_id")))
        if r.get("gate_id") and r["gate_id"] not in ids["gates"]: findings.append(_finding("FLIGHT_UNKNOWN_GATE", "error", f"gate {r['gate_id']!r} does not exist", field="gate_id", value=r["gate_id"]))
        if r.get("operation") not in ("arrival", "departure"):
            findings.append(_finding("ENUM_UNSUPPORTED", "error", "operation must be arrival or departure", field="operation", value=r.get("operation")))
    for r in tables["turnaround_requirements"]:
        if not r.get("flight_id") and not r.get("aircraft_id"):
            findings.append(_finding("REQUIREMENT_REFERENCE_MISSING", "error", "requirement must reference a flight_id or aircraft_id", field="flight_id"))
        if r.get("flight_id") and r["flight_id"] not in ids["flights"]: findings.append(_finding("REQUIREMENT_UNKNOWN_FLIGHT", "error", f"flight {r['flight_id']!r} does not exist", field="flight_id", value=r["flight_id"]))
        if r.get("aircraft_id") and r["aircraft_id"] not in ids["aircraft"]: findings.append(_finding("REQUIREMENT_UNKNOWN_AIRCRAFT", "error", f"aircraft {r['aircraft_id']!r} does not exist", field="aircraft_id", value=r["aircraft_id"]))
        if r.get("equipment_id") and r["equipment_id"] not in ids["equipment"]: findings.append(_finding("REQUIREMENT_UNKNOWN_EQUIPMENT", "error", f"equipment {r['equipment_id']!r} does not exist", field="equipment_id", value=r["equipment_id"]))
        if r.get("equipment_id") in ids["equipment"] and r.get("required_equipment_type"):
            equipment = next(x for x in tables["equipment"] if x.get("equipment_id") == r["equipment_id"])
            wanted = re.sub(r"[ -]+", "_", str(r["required_equipment_type"]).strip().lower())
            if equipment.get("type") != wanted:
                findings.append(_finding("REQUIREMENT_EQUIPMENT_TYPE_MISMATCH", "error", f"equipment {equipment['equipment_id']!r} is {equipment.get('type')!r}, not required type {wanted!r}", field="required_equipment_type", value=wanted))
    known_disruptions = {"equipment_outage", "route_closure", "delayed_service", "flight_delay", "gate_unavailability"}
    for r in tables["disruptions"]:
        if r.get("type") not in known_disruptions: findings.append(_finding("DISRUPTION_TYPE_UNSUPPORTED", "error", f"unsupported disruption type {r.get('type')!r}", field="type", value=r.get("type")))
        target_field = {"equipment_outage": "equipment_id", "route_closure": "resource_id", "flight_delay": "flight_id", "gate_unavailability": "gate_id"}.get(r.get("type"))
        if target_field and not r.get(target_field): findings.append(_finding("DISRUPTION_TARGET_MISSING", "error", f"{r.get('type')} requires {target_field}", field=target_field))
        if r.get("type") == "delayed_service" and not (r.get("flight_id") or r.get("requirement_id")):
            findings.append(_finding("DISRUPTION_TARGET_MISSING", "error", "delayed_service requires flight_id or requirement_id", field="flight_id"))
        target = {"equipment_outage": ("equipment_id", "equipment"), "gate_unavailability": ("gate_id", "gates"), "flight_delay": ("flight_id", "flights")}.get(r.get("type"))
        if target and r.get(target[0]) not in ids[target[1]]: findings.append(_finding("DISRUPTION_UNKNOWN_TARGET", "error", f"{target[0]} {r.get(target[0])!r} does not exist", field=target[0], value=r.get(target[0])))
        if r.get("type") == "delayed_service" and r.get("flight_id") and r["flight_id"] not in ids["flights"]:
            findings.append(_finding("DISRUPTION_UNKNOWN_TARGET", "error", f"flight_id {r['flight_id']!r} does not exist", field="flight_id", value=r["flight_id"]))
        if r.get("type") == "delayed_service" and r.get("requirement_id") and r["requirement_id"] not in ids["turnaround_requirements"]:
            findings.append(_finding("DISRUPTION_UNKNOWN_TARGET", "error", f"requirement_id {r['requirement_id']!r} does not exist", field="requirement_id", value=r["requirement_id"]))
        if r.get("end_time") and r.get("start_time") and r["end_time"] <= r["start_time"]: findings.append(_finding("INTERVAL_INVALID", "error", "end_time must be later than start_time", field="end_time", value=r["end_time"]))
        if r.get("type") == "route_closure": findings.append(_finding("ROUTE_REFERENCE_UNRESOLVED", "info", "route/resource ID is structurally retained but cannot be checked against airport geometry in Goal 24A", field="resource_id", value=r.get("resource_id")))
    for gate_id in sorted({r.get("gate_id") for r in tables["flights"] if r.get("gate_id")}):
        assigned = [r for r in tables["flights"] if r.get("gate_id") == gate_id]
        if len(assigned) > 1 and all(r.get("occupancy_start") and r.get("occupancy_end") for r in assigned):
            for i, left in enumerate(assigned):
                if left["occupancy_end"] <= left["occupancy_start"]:
                    findings.append(_finding("INTERVAL_INVALID", "error", "occupancy_end must be later than occupancy_start", field="occupancy_end", value=left["occupancy_end"]))
                for right in assigned[i + 1:]:
                    if left["occupancy_start"] < right["occupancy_end"] and right["occupancy_start"] < left["occupancy_end"]:
                        findings.append(_finding("GATE_ASSIGNMENT_OVERLAP", "error", f"flights {left.get('flight_id')!r} and {right.get('flight_id')!r} have overlapping occupancy at gate {gate_id!r}", field="gate_id", value=gate_id))
        elif len(assigned) > 1:
            findings.append(_finding("GATE_CONFLICT_UNDETERMINED", "info", f"multiple flights use gate {gate_id}; occupancy intervals are not supplied, so overlap cannot be assessed", field="gate_id", value=gate_id))
    for table in tables: tables[table].sort(key=lambda r: tuple(str(r.get(k, "")) for k in sorted(r)))
    findings.sort(key=lambda f: (f["severity"], f["code"], f["source"], f["record"] or 0, f["field"], str(f["value"])))
    report = {"schema_version": VERSION, "status": "error" if any(f["severity"] == "error" for f in findings) else "valid", "counts": counts, "error_count": sum(f["severity"] == "error" for f in findings), "warning_count": sum(f["severity"] == "warning" for f in findings), "info_count": sum(f["severity"] == "info" for f in findings), "findings": findings}
    data = {"airport": {k: v for k, v in airport.items() if v not in (None, "")}, "dataset": {"dataset_id": manifest.get("dataset_id", ""), "description": manifest.get("description", "")}, "sources": sorted(sources, key=lambda s: s["filename"]), **tables, "validation": report}
    return data, findings, counts

def canonical_bytes(data):
    return (json.dumps({"schema_version": VERSION, **data}, ensure_ascii=False, sort_keys=True, indent=2, allow_nan=False) + "\n").encode("utf-8")
