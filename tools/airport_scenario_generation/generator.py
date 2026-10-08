"""Goal 24B adapter. Generated JSON is valid YAML and loads through the native scenario loader."""
from __future__ import annotations

from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
from typing import Any

from . import VERSION

PACKAGE_SECTIONS = ("airport", "dataset", "sources", "flights", "aircraft", "gates", "equipment",
                    "turnaround_requirements", "disruptions", "validation")
REQUIRED_KEYS = {
    "airport": ("airport_id", "timezone"), "dataset": ("dataset_id",),
    "flights": ("flight_id", "operation", "scheduled_time", "aircraft_id"),
    "aircraft": ("aircraft_id", "type"), "gates": ("gate_id",),
    "equipment": ("equipment_id", "type", "status"),
    "turnaround_requirements": ("requirement_id", "service_type", "duration_minutes"),
    "disruptions": ("disruption_id", "type", "start_time"),
}


class GenerationError(ValueError):
    pass


def canonical_json(data: Any) -> bytes:
    return (json.dumps(data, ensure_ascii=False, sort_keys=True, indent=2, allow_nan=False) + "\n").encode("utf-8")


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def read_json(path: Path) -> tuple[Any, bytes]:
    try:
        raw = path.read_bytes()
        value = json.loads(raw.decode("utf-8"), parse_constant=lambda item: (_ for _ in ()).throw(ValueError(f"invalid number {item}")))
        return value, raw
    except (OSError, UnicodeError, ValueError) as exc:
        raise GenerationError(f"cannot read JSON {path}: {exc}") from exc


def timestamp(value: Any, context: str) -> datetime:
    if not isinstance(value, str):
        raise GenerationError(f"{context}: timestamp must be an ISO-8601 string")
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError as exc:
        raise GenerationError(f"{context}: invalid ISO-8601 timestamp {value!r}") from exc
    if parsed.tzinfo is None or parsed.utcoffset() is None:
        raise GenerationError(f"{context}: timestamp must include a UTC offset")
    return parsed.astimezone(timezone.utc)


def load_package(path: Path) -> tuple[dict[str, Any], bytes]:
    data, raw = read_json(path)
    if not isinstance(data, dict):
        raise GenerationError("canonical package root must be an object")
    version = str(data.get("schema_version", ""))
    if not re.fullmatch(r"\d+\.\d+(?:\.\d+)?", version):
        raise GenerationError(f"unsupported canonical schema version {version!r}; expected Goal 24A 1.x")
    if int(version.split(".")[0]) != 1:
        raise GenerationError(f"unsupported canonical major schema version {version!r}; supported major version is 1")
    missing = [key for key in PACKAGE_SECTIONS if key not in data]
    if missing:
        raise GenerationError("canonical package is missing required sections: " + ", ".join(missing))
    for section in ("airport", "dataset", "validation"):
        if not isinstance(data[section], dict):
            raise GenerationError(f"canonical section {section} must be an object")
    if data["validation"].get("status") != "valid" or int(data["validation"].get("error_count", 0)) != 0:
        raise GenerationError("Goal 24A package validation is not valid or reports blocking errors")
    for section in PACKAGE_SECTIONS:
        if section not in ("airport", "dataset", "validation") and not isinstance(data[section], list):
            raise GenerationError(f"canonical section {section} must be an array")
        if section in REQUIRED_KEYS and section not in ("airport", "dataset", "validation"):
            rows = data[section]
            if not rows:
                if section in ("flights", "aircraft", "gates"):
                    raise GenerationError(f"canonical required section {section} cannot be empty")
                continue
            seen: set[str] = set()
            for index, row in enumerate(rows):
                if not isinstance(row, dict):
                    raise GenerationError(f"{section}[{index}] must be an object")
                for key in REQUIRED_KEYS[section]:
                    if row.get(key) in (None, ""):
                        raise GenerationError(f"{section}[{index}] is missing required field {key}")
                id_key = {"flights": "flight_id", "aircraft": "aircraft_id", "gates": "gate_id", "equipment": "equipment_id",
                          "turnaround_requirements": "requirement_id", "disruptions": "disruption_id"}.get(section)
                if id_key:
                    identifier = str(row[id_key])
                    if identifier in seen:
                        raise GenerationError(f"duplicate {section} identifier {identifier!r}")
                    seen.add(identifier)
    if data["airport"].get("timezone") not in ("UTC", "Etc/UTC", "GMT"):
        raise GenerationError("canonical package timezone must already be normalized to UTC")
    flights = {str(row["flight_id"]): row for row in data["flights"]}
    aircraft = {str(row["aircraft_id"]) for row in data["aircraft"]}
    gates = {str(row["gate_id"]) for row in data["gates"]}
    equipment = {str(row["equipment_id"]) for row in data["equipment"]}
    requirements = {str(row["requirement_id"]): row for row in data["turnaround_requirements"]}
    for flight in flights.values():
        if str(flight["aircraft_id"]) not in aircraft:
            raise GenerationError(f"flight {flight['flight_id']!r} references unknown aircraft {flight['aircraft_id']!r}")
        if flight.get("gate_id") and str(flight["gate_id"]) not in gates:
            raise GenerationError(f"flight {flight['flight_id']!r} references unknown gate {flight['gate_id']!r}")
        timestamp(flight["scheduled_time"], f"flight {flight['flight_id']}.scheduled_time")
        for field in ("estimated_time", "actual_time"):
            if flight.get(field):
                timestamp(flight[field], f"flight {flight['flight_id']}.{field}")
    for req in requirements.values():
        if req.get("flight_id") and str(req["flight_id"]) not in flights:
            raise GenerationError(f"requirement {req['requirement_id']!r} references unknown flight {req['flight_id']!r}")
        if req.get("aircraft_id") and str(req["aircraft_id"]) not in aircraft:
            raise GenerationError(f"requirement {req['requirement_id']!r} references unknown aircraft {req['aircraft_id']!r}")
        if req.get("equipment_id") and str(req["equipment_id"]) not in equipment:
            raise GenerationError(f"requirement {req['requirement_id']!r} references unknown equipment {req['equipment_id']!r}")
        if float(req["duration_minutes"]) < 0:
            raise GenerationError(f"requirement {req['requirement_id']!r} duration_minutes cannot be negative")
    for disruption in data["disruptions"]:
        timestamp(disruption["start_time"], f"disruption {disruption['disruption_id']}.start_time")
        if disruption.get("end_time"):
            if timestamp(disruption["end_time"], f"disruption {disruption['disruption_id']}.end_time") <= timestamp(disruption["start_time"], "disruption start_time"):
                raise GenerationError(f"disruption {disruption['disruption_id']!r} has end_time not later than start_time")
    return data, raw


def load_mapping(path: Path) -> tuple[dict[str, Any], bytes, dict[str, Any], bytes]:
    mapping, raw = read_json(path)
    if not isinstance(mapping, dict) or mapping.get("schema_version") != "1.0":
        raise GenerationError("mapping must be a schema_version 1.0 JSON object")
    geometry_path = (path.parent / str(mapping.get("geometry_file", ""))).resolve()
    if not geometry_path.is_relative_to(path.parent.resolve()):
        raise GenerationError("geometry_file must stay within the mapping directory")
    geometry, geometry_raw = read_json(geometry_path)
    if not isinstance(geometry, dict):
        raise GenerationError("geometry file root must be an object")
    airport = geometry.get("airport", {})
    nodes = {str(x.get("id")) for x in airport.get("nodes", [])}
    edges = {str(x.get("id")) for x in airport.get("edges", [])}
    gate_resources = {str(x.get("id")): x for x in geometry.get("gates", [])}
    if not nodes or not edges or not gate_resources:
        raise GenerationError("geometry requires non-empty airport nodes, edges, and gates")
    gate_ids = mapping.get("gate_ids", {})
    if not isinstance(gate_ids, dict):
        raise GenerationError("gate_ids must be an object")
    mapped_gates = list(map(str, gate_ids.values()))
    if len(set(mapped_gates)) != len(mapped_gates):
        raise GenerationError("duplicate gate resource mapping in gate_ids")
    for canonical, target in gate_ids.items():
        if str(target) not in gate_resources:
            raise GenerationError(f"gate_ids maps {canonical!r} to unknown simulator gate {target!r}")
    for key, resources in (("route_ids", edges),):
        values = mapping.get(key, {})
        if not isinstance(values, dict):
            raise GenerationError(f"{key} must be an object")
        if len(set(map(str, values.values()))) != len(values):
            raise GenerationError(f"duplicate simulator resource mapping in {key}")
        for canonical, target in values.items():
            if str(target) not in resources:
                raise GenerationError(f"{key} maps {canonical!r} to unknown simulator resource {target!r}")
    for key in ("arrival_exit_node", "departure_handoff_node", "runway_node"):
        if str(mapping.get(key, "")) not in nodes:
            raise GenerationError(f"{key} references unknown simulator node {mapping.get(key)!r}")
    equipment_types = mapping.get("equipment_types", {})
    if not isinstance(equipment_types, dict):
        raise GenerationError("equipment_types must be an object")
    allowed_vehicle_types = {"fueling", "baggage", "catering", "deboarding", "cabin_cleaning", "baggage_load", "pushback_preparation"}
    for canonical, value in equipment_types.items():
        if not isinstance(value, dict):
            raise GenerationError(f"equipment_types.{canonical} must be an object")
        if value.get("type") not in allowed_vehicle_types:
            raise GenerationError(f"unsupported simulator vehicle type {value.get('type')!r} for canonical {canonical!r}")
        if value.get("initial_node") not in nodes:
            raise GenerationError(f"equipment_types.{canonical} initial_node references unknown simulator node {value.get('initial_node')!r}")
    service_types = mapping.get("service_types", {})
    allowed_services = {"fueling", "baggage", "deboarding", "catering", "cabin_cleaning", "baggage_load", "pushback_preparation"}
    if not isinstance(service_types, dict) or any(v not in allowed_services for v in service_types.values()):
        raise GenerationError("service_types contains an unsupported RampLab service type")
    return mapping, raw, geometry, geometry_raw


def seconds_from(value: str, epoch: datetime) -> int:
    delta = timestamp(value, "time") - epoch
    seconds = delta.total_seconds()
    if seconds < 0:
        raise GenerationError("time precedes the deterministic scenario epoch")
    return int(round(seconds))


def _warn(warnings: set[str], code: str, message: str) -> None:
    warnings.add(f"{code}: {message}")


def generate(package_path: Path, mapping_path: Path, output: Path, *, seed: int | None = None,
             without_disruptions: bool = False) -> dict[str, Any]:
    data, package_raw = load_package(package_path)
    mapping, mapping_raw, geometry, geometry_raw = load_mapping(mapping_path)
    seed = int(mapping.get("seed", 42) if seed is None else seed)
    warnings: set[str] = set()
    gates = {str(gate["gate_id"]): gate for gate in data["gates"]}
    for gate_id in sorted(gates):
        if gate_id not in mapping["gate_ids"]:
            raise GenerationError(f"missing required gate mapping for canonical gate {gate_id!r}")
    gate_resources = {str(gate["id"]): gate for gate in geometry["gates"]}
    nodes = {str(node["id"]) for node in geometry["airport"]["nodes"]}
    edges = {str(edge["id"]) for edge in geometry["airport"]["edges"]}
    for flight in data["flights"]:
        if flight.get("gate_id") not in mapping["gate_ids"]:
            raise GenerationError(f"flight {flight['flight_id']!r} gate {flight.get('gate_id')!r} has no simulator mapping")
    equipment_map = mapping["equipment_types"]
    equipment_rows = []
    equipment_identity = {}
    spawn_owners: dict[str, str] = {}
    for item in sorted(data["equipment"], key=lambda x: str(x["equipment_id"])):
        key, ident = str(item["type"]), str(item["equipment_id"])
        if key not in equipment_map:
            _warn(warnings, "EQUIPMENT_METADATA_ONLY", f"{ident} ({key}) has no safe RampLab vehicle class")
            continue
        if item.get("status") not in ("available", "active", "open"):
            _warn(warnings, "EQUIPMENT_NOT_ACTIVE", f"{ident} status {item.get('status')!r} was not added to the active fleet")
            continue
        adapter = equipment_map[key]
        node = str(item.get("initial_location", adapter["initial_node"]))
        if node not in nodes:
            raise GenerationError(f"equipment {ident!r} initial location {node!r} is not a simulator node")
        if node in spawn_owners:
            raise GenerationError(
                f"equipment {ident!r} shares simulator spawn node {node!r} with {spawn_owners[node]!r}; "
                "map active vehicles to distinct safe nodes"
            )
        spawn_owners[node] = ident
        vehicle_id = ident
        equipment_identity[ident] = vehicle_id
        equipment_rows.append({"id": vehicle_id, "name": ident, "type": adapter["type"], "depot_node": node, "speed_mps": 8.0})
    if not any(v["type"] == "fueling" for v in equipment_rows):
        raise GenerationError("generated fleet needs at least one mapped active fuel_vehicle")
    if not any(v["type"] == "baggage" for v in equipment_rows):
        raise GenerationError("generated fleet needs at least one mapped active baggage_vehicle")

    flights = sorted(data["flights"], key=lambda x: (timestamp(x["scheduled_time"], "flight time"), str(x["flight_id"])))
    canonical_disruptions = sorted(data["disruptions"], key=lambda x: str(x["disruption_id"]))
    disruptions = [] if without_disruptions else canonical_disruptions
    flight_delays = {}
    for disruption in disruptions:
        if disruption.get("type") == "flight_delay":
            flight_id = str(disruption.get("flight_id", ""))
            flight_delays[flight_id] = flight_delays.get(flight_id, 0.0) + float(disruption.get("delay_minutes", disruption.get("duration_minutes", 0)))
    operational_times = [timestamp(f.get("actual_time") or f.get("estimated_time") or f["scheduled_time"], f"flight {f['flight_id']}") for f in flights]
    # Keep the scenario epoch stable between matched control and disruption variants.
    operational_times += [timestamp(d["start_time"], f"disruption {d['disruption_id']}") for d in canonical_disruptions]
    epoch = min(operational_times)
    adjusted = {}
    for flight in flights:
        when = timestamp(flight.get("actual_time") or flight.get("estimated_time") or flight["scheduled_time"], f"flight {flight['flight_id']}")
        when += __import__("datetime").timedelta(seconds=round(flight_delays.get(str(flight["flight_id"]), 0.0) * 60))
        adjusted[str(flight["flight_id"])] = when
    by_aircraft: dict[str, list[dict[str, Any]]] = {}
    for flight in flights:
        by_aircraft.setdefault(str(flight["aircraft_id"]), []).append(flight)
    aircraft_meta = {str(x["aircraft_id"]): x for x in data["aircraft"]}
    requirements_by_aircraft: dict[str, list[dict[str, Any]]] = {}
    flight_to_aircraft = {str(x["flight_id"]): str(x["aircraft_id"]) for x in flights}
    for requirement in sorted(data["turnaround_requirements"], key=lambda x: str(x["requirement_id"])):
        target_aircraft = str(requirement.get("aircraft_id") or flight_to_aircraft.get(str(requirement.get("flight_id")), ""))
        if not target_aircraft:
            raise GenerationError(f"requirement {requirement['requirement_id']!r} has no resolved aircraft")
        service = str(requirement["service_type"])
        if service not in mapping["service_types"]:
            raise GenerationError(f"required turnaround service {service!r} ({requirement['requirement_id']}) has no RampLab equivalent")
        requirement["_sim_aircraft"] = target_aircraft
        requirements_by_aircraft.setdefault(target_aircraft, []).append(requirement)

    simulator_aircraft = []
    identity_flights: dict[str, Any] = {}
    requirement_task = {}
    for aircraft_id in sorted(by_aircraft):
        legs = sorted(by_aircraft[aircraft_id], key=lambda f: (adjusted[str(f["flight_id"])], str(f["flight_id"])))
        arrivals = [f for f in legs if f["operation"] == "arrival"]
        departures = [f for f in legs if f["operation"] == "departure"]
        if len(arrivals) > 1 or len(departures) > 1 or (not arrivals and not departures):
            raise GenerationError(f"aircraft {aircraft_id!r} must have at most one arrival and one departure leg in one scenario")
        arrival = arrivals[0] if arrivals else None
        departure = departures[0] if departures else None
        gate_source = arrival or departure
        if arrival and departure and str(arrival.get("gate_id")) != str(departure.get("gate_id")):
            raise GenerationError(f"paired flights {arrival['flight_id']!r} and {departure['flight_id']!r} have different gates")
        gate_id = str(gate_source.get("gate_id", ""))
        gate_resource_id = str(mapping["gate_ids"][gate_id])
        gate_resource = gate_resources[gate_resource_id]
        task_rows = []
        reqs = requirements_by_aircraft.get(aircraft_id, [])
        seen_services = set()
        req_to_task = {}
        for req in reqs:
            target_type = mapping["service_types"][str(req["service_type"])]
            if target_type in seen_services:
                raise GenerationError(f"aircraft {aircraft_id!r} has duplicate mapped service {target_type!r}; simulator permits one task per service class")
            seen_services.add(target_type)
            task_id = str(req["requirement_id"])
            duration_seconds = int(round(float(req["duration_minutes"]) * 60))
            if duration_seconds <= 0:
                raise GenerationError(f"required task {task_id!r} has zero duration after simulator-second conversion")
            task = {"id": task_id, "type": target_type, "duration_seconds": duration_seconds}
            if req.get("earliest_start"):
                task["earliest_start_seconds"] = seconds_from(str(req["earliest_start"]), epoch)
            prerequisite = req.get("depends_on") or req.get("dependency_requirement_id")
            if prerequisite:
                task["prerequisites"] = [str(prerequisite)]
            task_rows.append(task)
            req_to_task[task_id] = task_id
            requirement_task[task_id] = {"aircraft": aircraft_id, "task": task_id}
        if arrival and departure:
            operation_type = "arrival_turnaround"
            arr_time, dep_time = adjusted[str(arrival["flight_id"])], adjusted[str(departure["flight_id"])]
        elif arrival:
            operation_type = "arrival_turnaround" if task_rows else "arrival"
            arr_time = adjusted[str(arrival["flight_id"])]
            dep_time = arr_time + __import__("datetime").timedelta(seconds=sum(t["duration_seconds"] for t in task_rows) + 120)
        else:
            operation_type = "turnaround"
            dep_time = adjusted[str(departure["flight_id"])]
            arr_time = dep_time
        if dep_time < arr_time:
            raise GenerationError(f"aircraft {aircraft_id!r} departure time precedes its arrival")
        simulator_aircraft.append({"id": aircraft_id, "operation_type": operation_type, "gate": gate_resource_id,
            "arrival_exit": mapping["arrival_exit_node"], "turnaround_id": aircraft_id,
            "scheduled_arrival_seconds": int(round((arr_time - epoch).total_seconds())),
            "scheduled_departure_seconds": int(round((dep_time - epoch).total_seconds())),
            "target_off_block_seconds": int(round((dep_time - epoch).total_seconds())), "service_tasks": task_rows})
        for leg in legs:
            identity_flights[str(leg["flight_id"])] = {"simulator_aircraft_id": aircraft_id, "operation": leg["operation"],
                "canonical_aircraft_id": aircraft_id, "canonical_gate_id": gate_id, "simulator_gate_id": gate_resource_id}

    road_events, vehicle_outages, turnaround_disruptions = [], [], []
    disruption_identity = {}
    for disruption in disruptions:
        did, kind = str(disruption["disruption_id"]), str(disruption["type"])
        when = seconds_from(str(disruption["start_time"]), epoch)
        if kind == "route_closure":
            route = str(disruption.get("resource_id", ""))
            if route not in mapping["route_ids"]:
                _warn(warnings, "UNRESOLVED_ROUTE_CLOSURE", f"{did} route {route!r} has no geometry mapping")
                continue
            edge = str(mapping["route_ids"][route])
            if edge not in edges:
                raise GenerationError(f"route mapping for {route!r} no longer resolves to geometry edge {edge!r}")
            road_events.append({"time_seconds": when, "edge": edge, "enabled": False})
            if disruption.get("end_time"):
                reopen = seconds_from(str(disruption["end_time"]), epoch)
                road_events.append({"time_seconds": reopen, "edge": edge, "enabled": True})
            disruption_identity[did] = {"type": kind, "simulator_edge_id": edge, "time_seconds": when,
                "end_time_seconds": seconds_from(str(disruption["end_time"]), epoch) if disruption.get("end_time") else None}
        elif kind == "equipment_outage":
            equipment_id = str(disruption.get("equipment_id", ""))
            if disruption.get("duration_minutes") is not None or disruption.get("end_time"):
                _warn(warnings, "UNSUPPORTED_FINITE_EQUIPMENT_OUTAGE", f"{did} has a finite outage window; RampLab vehicle outages do not restore a vehicle at an end time")
                continue
            if equipment_id not in equipment_identity:
                _warn(warnings, "UNSUPPORTED_EQUIPMENT_OUTAGE", f"{did} targets unmapped equipment {equipment_id!r}")
                continue
            vehicle_outages.append({"time_seconds": when, "vehicle": equipment_identity[equipment_id]})
            disruption_identity[did] = {"type": kind, "simulator_vehicle_id": equipment_identity[equipment_id], "time_seconds": when}
        elif kind == "delayed_service":
            req_id = str(disruption.get("requirement_id", ""))
            if not req_id and disruption.get("flight_id"):
                target_flight = str(disruption["flight_id"])
                possible = [r for r in data["turnaround_requirements"] if str(r.get("flight_id")) == target_flight]
                if len(possible) == 1:
                    req_id = str(possible[0]["requirement_id"])
            target = requirement_task.get(req_id)
            if not target:
                _warn(warnings, "UNRESOLVED_SERVICE_DELAY", f"{did} does not resolve to one generated turnaround task")
                continue
            delay_seconds = int(round(float(disruption.get("delay_minutes", disruption.get("duration_minutes", 0))) * 60))
            if delay_seconds <= 0:
                _warn(warnings, "INVALID_SERVICE_DELAY", f"{did} has no positive duration to apply")
                continue
            required = next((r for r in data["turnaround_requirements"] if str(r["requirement_id"]) == req_id), None)
            duration_seconds = int(round(float(required["duration_minutes"]) * 60)) + delay_seconds
            turnaround_disruptions.append({"time_seconds": when, "aircraft": target["aircraft"], "task": target["task"], "duration_seconds": duration_seconds})
            disruption_identity[did] = {"type": kind, **target, "time_seconds": when}
        elif kind == "flight_delay":
            target = str(disruption.get("flight_id", ""))
            if target not in identity_flights:
                _warn(warnings, "UNRESOLVED_FLIGHT_DELAY", f"{did} references flight {target!r} without an operation")
            else:
                disruption_identity[did] = {"type": kind, "canonical_flight_id": target, "delay_minutes": flight_delays.get(target, 0)}
        elif kind == "gate_unavailability":
            _warn(warnings, "UNSUPPORTED_GATE_UNAVAILABILITY", f"{did} targets {disruption.get('gate_id')!r}; no safe gate outage behavior exists")
        else:
            _warn(warnings, "UNSUPPORTED_DISRUPTION", f"{did} type {kind!r} has no generated simulator equivalent")

    service_durations = {"fueling": 300, "baggage": 240, "deboarding": 120, "catering": 300,
                         "cabin_cleaning": 180, "baggage_load": 240, "pushback_preparation": 120}
    scenario_name = "airport_" + re.sub(r"[^A-Za-z0-9_]+", "_", str(data["airport"]["airport_id"])).strip("_").lower()
    scenario = {"name": scenario_name, "default_seed": seed,
        "airport": geometry["airport"],
        "gates": geometry["gates"],
        "fleet": {"vehicles": equipment_rows},
        "aircraft": simulator_aircraft,
        "surface_operations": geometry["surface_operations"],
        "service_durations_seconds": service_durations}
    if road_events:
        scenario["road_events"] = sorted(road_events, key=lambda x: (x["time_seconds"], x["edge"], x["enabled"]))
    if vehicle_outages:
        scenario["vehicle_outages"] = sorted(vehicle_outages, key=lambda x: (x["time_seconds"], x["vehicle"]))
    if turnaround_disruptions:
        scenario["turnaround_disruptions"] = sorted(turnaround_disruptions, key=lambda x: (x["time_seconds"], x["aircraft"], x["task"]))
    for item in data["aircraft"]:
        for field in ("registration", "model", "icao_type", "category", "wingspan", "length"):
            if item.get(field) is not None:
                _warn(warnings, "AIRCRAFT_METADATA_ONLY", f"{item['aircraft_id']}.{field} preserved in manifest; movement behavior is unchanged")
    for gate in data["gates"]:
        for field in ("status", "available", "compatible_aircraft_class", "location"):
            if field in gate and gate[field] not in ("available", True, None):
                _warn(warnings, "GATE_METADATA_ONLY", f"{gate['gate_id']}.{field}={gate[field]!r} does not alter simulator gate behavior")

    # Validate references in the emitted format before any files are written.
    simulator_gate_ids = {str(x["id"]) for x in scenario["gates"]}
    simulator_node_ids = {str(x["id"]) for x in scenario["airport"]["nodes"]}
    if any(str(x["gate"]) not in simulator_gate_ids for x in simulator_aircraft):
        raise GenerationError("generated aircraft references an unknown simulator gate")
    if any(str(x["depot_node"]) not in simulator_node_ids for x in equipment_rows):
        raise GenerationError("generated vehicle references an unknown simulator node")
    generated_scenario = canonical_json(scenario)
    package_hash, mapping_hash, geometry_hash = sha256(package_raw), sha256(mapping_raw), sha256(geometry_raw)
    option_bytes = canonical_json({"seed": seed, "without_disruptions": without_disruptions})
    scenario_id = "airport-" + sha256(package_raw + mapping_raw + geometry_raw + option_bytes)[:16]
    identity_map = {"flights": identity_flights,
        "aircraft": {str(x["aircraft_id"]): str(x["aircraft_id"]) for x in data["aircraft"] if str(x["aircraft_id"]) in by_aircraft},
        "gates": {key: str(value) for key, value in sorted(mapping["gate_ids"].items()) if key in gates},
        "equipment": equipment_identity, "turnaround_requirements": requirement_task,
        "disruptions": disruption_identity}
    identity_bytes = canonical_json(identity_map)
    support, _ = read_json(Path(__file__).with_name("support_matrix.json"))
    warnings_list = sorted(warnings)
    unsupported_metadata = {"aircraft": [str(x["aircraft_id"]) for x in data["aircraft"]],
                            "equipment_without_simulator_vehicle": sorted(set(str(x["equipment_id"]) for x in data["equipment"]) - set(equipment_identity))}
    files = {"scenario.json": sha256(generated_scenario), "identity-map.json": sha256(identity_bytes),
             "support-matrix.json": sha256(canonical_json(support))}
    manifest = {"generator_schema_version": VERSION, "source_canonical_sha256": package_hash,
        "source_canonical_schema_version": data["schema_version"], "source_dataset_id": data["dataset"].get("dataset_id"),
        "airport": data["airport"], "mapping_configuration_sha256": mapping_hash,
        "geometry_configuration_sha256": geometry_hash, "generated_scenario_id": scenario_id,
        "scenario_name": scenario_name, "seed": seed, "scenario_epoch_utc": epoch.isoformat().replace("+00:00", "Z"),
        "entities": {"flights": len(identity_flights), "aircraft": len(simulator_aircraft), "gates": len(identity_map["gates"]),
                     "ground_vehicles": len(equipment_rows), "turnaround_tasks": sum(len(x["service_tasks"]) for x in simulator_aircraft)},
        "generated_disruptions": {"route_closures": sum(1 for x in road_events if not x["enabled"]), "equipment_outages": len(vehicle_outages),
                                  "delayed_services": len(turnaround_disruptions), "flight_delays": len(flight_delays)},
        "warnings": warnings_list, "unsupported_metadata": unsupported_metadata,
        "source_provenance": data["sources"], "identity_map_file": "identity-map.json",
        "output_file_sha256": files}
    manifest_bytes = canonical_json(manifest)
    output.mkdir(parents=True, exist_ok=True)
    outputs = {"scenario.json": generated_scenario, "identity-map.json": identity_bytes,
               "support-matrix.json": canonical_json(support), "manifest.json": manifest_bytes}
    for name, contents in outputs.items():
        (output / name).write_bytes(contents)
    return {"scenario": scenario, "manifest": manifest, "output_dir": str(output)}


def validate_generated(output: Path) -> dict[str, Any]:
    scenario, _ = read_json(output / "scenario.json")
    manifest, _ = read_json(output / "manifest.json")
    identity, _ = read_json(output / "identity-map.json")
    if not isinstance(scenario, dict) or not isinstance(manifest, dict) or not isinstance(identity, dict):
        raise GenerationError("generated scenario, manifest, and identity map must be JSON objects")
    if not scenario.get("name") or not isinstance(scenario.get("aircraft"), list) or not scenario["aircraft"]:
        raise GenerationError("generated scenario is missing a name or aircraft")
    node_ids = {str(x["id"]) for x in scenario.get("airport", {}).get("nodes", [])}
    edge_ids = {str(x["id"]) for x in scenario.get("airport", {}).get("edges", [])}
    gate_ids = {str(x["id"]) for x in scenario.get("gates", [])}
    vehicle_ids = {str(x["id"]) for x in scenario.get("fleet", {}).get("vehicles", [])}
    aircraft_ids = {str(x["id"]) for x in scenario["aircraft"]}
    if not node_ids or not edge_ids or not gate_ids or not vehicle_ids:
        raise GenerationError("generated scenario is missing simulator geometry, gates, or vehicles")
    if any(str(x.get("gate")) not in gate_ids for x in scenario["aircraft"]):
        raise GenerationError("generated aircraft references unknown gate")
    for vehicle in scenario["fleet"]["vehicles"]:
        if str(vehicle.get("depot_node")) not in node_ids:
            raise GenerationError(f"vehicle {vehicle.get('id')!r} references unknown node")
    for aircraft in scenario["aircraft"]:
        tasks = aircraft.get("service_tasks", [])
        ids = {str(x.get("id")) for x in tasks}
        for task in tasks:
            if any(str(dep) not in ids for dep in task.get("prerequisites", [])):
                raise GenerationError(f"task {task.get('id')!r} references missing prerequisite")
    for event in scenario.get("road_events", []):
        if event.get("edge") not in edge_ids:
            raise GenerationError("road event references unknown edge")
    for event in scenario.get("vehicle_outages", []):
        if event.get("vehicle") not in vehicle_ids:
            raise GenerationError("vehicle outage references unknown vehicle")
    for event in scenario.get("turnaround_disruptions", []):
        if event.get("aircraft") not in aircraft_ids:
            raise GenerationError("turnaround disruption references unknown aircraft")
        tasks = {str(x.get("id")) for x in next(a for a in scenario["aircraft"] if a["id"] == event["aircraft"]).get("service_tasks", [])}
        if str(event.get("task")) not in tasks:
            raise GenerationError("turnaround disruption references unknown task")
    for name, digest in manifest.get("output_file_sha256", {}).items():
        if sha256((output / name).read_bytes()) != digest:
            raise GenerationError(f"generated output hash mismatch for {name}")
    if not identity.get("flights") or len(identity["flights"]) != manifest.get("entities", {}).get("flights"):
        raise GenerationError("identity map does not cover every generated canonical flight")
    return {"valid": True, "scenario": scenario["name"], "aircraft": len(aircraft_ids), "flights": len(identity["flights"])}


def inspect_generated(output: Path) -> dict[str, Any]:
    scenario, scenario_raw = read_json(output / "scenario.json")
    manifest, _ = read_json(output / "manifest.json")
    identity, _ = read_json(output / "identity-map.json")
    return {"source_canonical_sha256": manifest["source_canonical_sha256"],
        "mapping_configuration_sha256": manifest["mapping_configuration_sha256"],
        "airport": manifest["airport"], "flights_mapped": len(identity.get("flights", {})),
        "aircraft_generated": len(scenario.get("aircraft", [])), "gates_resolved": len(identity.get("gates", {})),
        "vehicles_generated": len(scenario.get("fleet", {}).get("vehicles", [])),
        "turnaround_tasks_generated": sum(len(x.get("service_tasks", [])) for x in scenario.get("aircraft", [])),
        "disruptions_generated": manifest.get("generated_disruptions", {}), "warnings": manifest.get("warnings", []),
        "output_files": sorted([p.name for p in output.iterdir() if p.is_file()]),
        "scenario_sha256": sha256(scenario_raw)}
