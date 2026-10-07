#!/usr/bin/env python3
"""Analyze existing RampLab experiment output without changing its schema."""

from __future__ import annotations

import argparse
import csv
import json
import math
import statistics
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any


@dataclass(frozen=True)
class Metric:
    label: str
    unit: str = ""
    category: str = "operations"
    lower_is_better: bool = True


METRICS = {
    "turnaround_minutes_mean": Metric("Mean turnaround", "min"),
    "departure_delay_minutes_mean": Metric("Mean departure delay", "min"),
    "service_waiting_minutes_mean": Metric("Mean service waiting", "min"),
    "fuel_utilization_mean": Metric("Fuel resource utilization", "ratio", "operations", False),
    "baggage_utilization_mean": Metric("Baggage resource utilization", "ratio", "operations", False),
    "mean_delayed_aircraft": Metric("Mean delayed aircraft", "aircraft"),
    "probability_any_delay": Metric("Probability of any delay", "ratio"),
    "mean_turnaround_duration_seconds": Metric("Mean turnaround", "s"),
    "mean_departure_delay_seconds": Metric("Mean departure delay", "s"),
    "maximum_turnaround_seconds": Metric("Maximum turnaround", "s"),
    "maximum_departure_delay_seconds": Metric("Maximum departure delay", "s"),
    "total_service_task_wait_seconds": Metric("Service task queueing", "s"),
    "maximum_service_task_wait_seconds": Metric("Maximum service task wait", "s"),
    "task_reassignments": Metric("Task reassignments", "count"),
    "disruption_triggered_replans": Metric("Disruption replans", "count"),
    "unresolved_service_requests": Metric("Unresolved service requests", "count", "safety"),
    "fleet_collisions": Metric("Fleet collisions", "count", "safety"),
    "fleet_minimum_separation_m": Metric("Fleet minimum separation", "m", "safety", False),
    "fleet_reservation_contentions": Metric("Fleet reservation contentions", "count"),
    "fleet_outstanding_reservations": Metric("Outstanding reservations", "count", "safety"),
    "fleet_unfinished_requests": Metric("Unfinished fleet requests", "count", "safety"),
    "fleet_reassignments": Metric("Fleet reassignments", "count"),
    "fleet_requests_created": Metric("Fleet requests created", "count", "throughput", False),
    "fleet_requests_completed": Metric("Fleet requests completed", "count", "throughput", False),
    "fleet_requests_failed": Metric("Fleet requests failed", "count", "safety"),
    "surface_departed_aircraft": Metric("Completed departures", "count", "throughput", False),
    "surface_arrived_aircraft": Metric("Completed arrivals", "count", "throughput", False),
    "surface_total_aircraft": Metric("Surface aircraft", "count", "throughput", False),
    "runway_operations_completed": Metric("Runway operations completed", "count", "throughput", False),
    "maximum_runway_queue_depth": Metric("Maximum runway queue", "aircraft"),
    "arrival_runway_wait_seconds": Metric("Arrival runway wait", "s"),
    "departure_runway_wait_seconds": Metric("Departure runway wait", "s"),
    "average_runway_wait_seconds": Metric("Average runway wait", "s"),
    "runway_utilization": Metric("Runway utilization", "ratio", "operations", False),
    "arrival_taxi_distance_m": Metric("Arrival taxi distance", "m"),
    "departure_taxi_distance_m": Metric("Departure taxi distance", "m"),
    "arrival_taxi_seconds": Metric("Arrival taxi time", "s"),
    "departure_taxi_seconds": Metric("Departure taxi time", "s"),
    "surface_reroutes": Metric("Surface reroutes", "count"),
    "surface_wait_events": Metric("Surface wait events", "count"),
    "surface_wait_seconds": Metric("Surface queueing", "s"),
    "surface_taxi_distance_m": Metric("Total taxi distance", "m"),
    "surface_taxi_seconds": Metric("Total taxi time", "s"),
    "runway_queue_seconds": Metric("Runway queueing", "s"),
    "surface_safe_failures": Metric("Surface safety failures", "count", "safety"),
    "max_simultaneous_taxiing_aircraft": Metric("Peak taxiing aircraft", "aircraft", "throughput", False),
    "surface_aircraft_aircraft_collisions": Metric("Aircraft collisions", "count", "safety"),
    "surface_aircraft_ground_collisions": Metric("Aircraft-ground collisions", "count", "safety"),
    "minimum_aircraft_separation_m": Metric("Minimum aircraft separation", "m", "safety", False),
    "minimum_aircraft_ground_separation_m": Metric("Minimum aircraft-ground separation", "m", "safety", False),
    "total_turnarounds": Metric("Total turnarounds", "count", "throughput", False),
    "completed_turnarounds": Metric("Completed turnarounds", "count", "throughput", False),
    "delayed_turnarounds": Metric("Delayed turnarounds", "count"),
    "road_closure_events": Metric("Road closure events", "count", "disruptions"),
    "failed_or_timed_out_turnarounds": Metric("Failed or timed out turnarounds", "count", "safety"),
    "on_time_departure_rate": Metric("On-time departure rate", "ratio", "throughput", False),
    "simulated_duration_seconds": Metric("Simulation duration", "s"),
    "avg_turnaround_minutes": Metric("Mean turnaround", "min"),
    "avg_departure_delay_minutes": Metric("Mean departure delay", "min"),
    "avg_service_waiting_minutes": Metric("Mean service waiting", "min"),
    "delayed_aircraft": Metric("Delayed aircraft", "count"),
    "aircraft_count": Metric("Aircraft count", "count", "throughput", False),
    "fuel_utilization": Metric("Fuel resource utilization", "ratio", "operations", False),
    "baggage_utilization": Metric("Baggage resource utilization", "ratio", "operations", False),
}

@dataclass
class Run:
    source: str
    name: str
    identity: dict[str, Any] = field(default_factory=dict)
    metrics: dict[str, float] = field(default_factory=dict)
    events: list[Any] = field(default_factory=list)
    raw: dict[str, Any] = field(default_factory=dict)
    warnings: list[str] = field(default_factory=list)


def _finite_number(value: Any) -> float | None:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    number = float(value)
    return number if math.isfinite(number) else None


def _aggregate(rows: list[dict[str, Any]]) -> dict[str, float]:
    found: dict[str, list[float]] = {}
    for row in rows:
        for key, value in row.items():
            if key in METRICS and (number := _finite_number(value)) is not None:
                found.setdefault(key, []).append(number)
    return {key: statistics.fmean(values) for key, values in found.items()}


def load_run(path: str | Path) -> Run:
    source = Path(path).resolve()
    suffix = source.suffix.lower()
    if suffix in {".json", ".jsonl"}:
        if suffix == ".jsonl":
            events = [json.loads(line) for line in source.read_text(encoding="utf-8").splitlines() if line.strip()]
            counts: dict[str, float] = {}
            for event in events:
                kind = event.get("type", event.get("event", "unknown")) if isinstance(event, dict) else "unknown"
                counts[str(kind)] = counts.get(str(kind), 0) + 1
            return Run(str(source), source.stem, metrics={}, events=events, raw={"event_counts": counts})
        data = json.loads(source.read_text(encoding="utf-8"))
        if not isinstance(data, dict):
            raise ValueError("JSON input must contain an object")
        if isinstance(data.get("runs"), list):
            rows = data["runs"]
            if not rows:
                rows = []
            metrics = _aggregate(rows)
            sibling = source.with_name("runs.csv")
            if sibling.is_file():
                csv_rows = _read_csv(sibling)
                metrics.update(_aggregate(csv_rows))
                rows = _merge_json_csv(rows, csv_rows)
            identity = {key: data[key] for key in ("experiment_name", "source_scenario", "schema_version", "run_count") if key in data}
            if len(rows) == 1:
                identity.update({k: rows[0][k] for k in ("seed", "case_id", "ordinal") if k in rows[0]})
            return Run(str(source), str(data.get("experiment_name", source.stem)), identity, metrics,
                       events=data.get("events", []), raw={**data, "runs": rows})
        metrics = _aggregate([data])
        identity = {k: v for k, v in data.items() if k in {"scenario_name", "scenario", "experiment_name", "seed", "case_id", "ordinal"}}
        name = str(identity.get("scenario_name", identity.get("scenario", identity.get("experiment_name", source.stem))))
        return Run(str(source), name, identity, metrics, data.get("events", []), data)
    if suffix == ".csv":
        rows = _read_csv(source)
        if not rows:
            raise ValueError("CSV input has no data rows")
        return Run(str(source), str(rows[0].get("scenario", source.stem)), _csv_identity(rows), _aggregate(rows), raw={"rows": rows})
    raise ValueError(f"unsupported input format: {suffix}; expected JSON, CSV, or JSONL")


def _read_csv(path: Path) -> list[dict[str, Any]]:
    rows = []
    with path.open(newline="", encoding="utf-8-sig") as handle:
        for row in csv.DictReader(handle):
            converted: dict[str, Any] = {}
            for key, value in row.items():
                if value is None or value == "":
                    continue
                number = _finite_number(float(value)) if key in METRICS or key in {"seed", "ordinal"} else None
                converted[key] = number if number is not None else value
            rows.append(converted)
    return rows


def _csv_identity(rows: list[dict[str, Any]]) -> dict[str, Any]:
    identity = {key: rows[0][key] for key in ("scenario", "seed", "case_id") if key in rows[0]}
    if len(rows) > 1:
        identity["row_count"] = len(rows)
    return identity


def _merge_json_csv(json_rows: list[dict[str, Any]], csv_rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    lookup = {str(row.get("ordinal", "")): row for row in csv_rows}
    merged = []
    for index, row in enumerate(json_rows):
        extra = lookup.get(str(row.get("ordinal", index + 1)), {})
        merged.append({**row, **extra})
    return merged


def compare(left: Run, right: Run) -> dict[str, Any]:
    shared = sorted(set(left.metrics) & set(right.metrics))
    deltas = []
    for key in shared:
        baseline, comparison = left.metrics[key], right.metrics[key]
        difference = comparison - baseline
        percent = difference / abs(baseline) * 100 if baseline != 0 else None
        definition = METRICS[key]
        impact = "unchanged" if difference == 0 else ("regression" if difference * (1 if definition.lower_is_better else -1) > 0 else "improvement")
        deltas.append({"key": key, "label": definition.label, "unit": definition.unit,
                       "category": definition.category, "baseline": baseline, "comparison": comparison,
                       "difference": difference, "percent_difference": percent, "impact": impact})
    ranked = sorted((d for d in deltas if d["difference"] != 0),
                    key=lambda d: (-abs(d["difference"] / max(abs(d["baseline"]), 1)), d["category"], d["key"]))
    return {"schema_version": 1, "baseline": {"name": left.name, "source": left.source, "identity": left.identity},
            "comparison": {"name": right.name, "source": right.source, "identity": right.identity},
            "shared_metric_count": len(shared), "metrics": deltas,
            "operational_impact": ranked[:10], "safety": assess_safety(left, right),
            "compatibility": {"comparable": bool(shared), "mismatched_identity": _identity_mismatch(left, right)}}


def _identity_mismatch(left: Run, right: Run) -> list[str]:
    keys = sorted(set(left.identity) & set(right.identity))
    return [key for key in keys if left.identity[key] != right.identity[key] and key not in {"experiment_name", "source_scenario"}]


def assess_safety(left: Run, right: Run) -> dict[str, Any]:
    safety_keys = [k for k, v in METRICS.items() if v.category == "safety" and k in left.metrics and k in right.metrics]
    flags = []
    collision_keys = ["fleet_collisions", "surface_aircraft_aircraft_collisions", "surface_aircraft_ground_collisions"]
    before = sum(left.metrics.get(k, 0) for k in collision_keys if k in left.metrics)
    after = sum(right.metrics.get(k, 0) for k in collision_keys if k in right.metrics)
    if after == 0 and before == 0 and any(k in left.metrics and k in right.metrics for k in collision_keys):
        flags.append({"status": "no_regression", "finding": "Zero collisions recorded in both runs."})
    elif after > before:
        flags.append({"status": "regression", "finding": f"Collisions increased from {before:g} to {after:g}."})
    for key in safety_keys:
        if key in collision_keys or key not in left.metrics: continue
        a, b = left.metrics[key], right.metrics[key]
        if key.endswith("separation_m") and b < a:
            flags.append({"status": "regression", "finding": f"{METRICS[key].label} decreased from {a:g} to {b:g} m."})
        elif key not in collision_keys and b > a and not key.endswith("separation_m"):
            flags.append({"status": "regression", "finding": f"{METRICS[key].label} increased from {a:g} to {b:g}."})
    collisions_observed = any(k in left.metrics and k in right.metrics for k in collision_keys)
    if not collisions_observed:
        flags.append({"status": "unknown", "finding": "Collision metrics are absent; safety cannot be declared from the available data."})
    elif not safety_keys:
        flags.append({"status": "unknown", "finding": "Insufficient shared safety metrics; safety cannot be assessed."})
    if any(k in left.metrics and k in right.metrics and right.metrics[k] > left.metrics[k]
           for k in ("failed_or_timed_out_turnarounds", "surface_safe_failures", "fleet_requests_failed")):
        flags.append({"status": "regression", "finding": "Failures or timeouts increased."})
    incomplete = [k for k in ("completed_turnarounds", "total_turnarounds") if k in left.metrics and k in right.metrics]
    if len(incomplete) == 2 and right.metrics[incomplete[0]] < right.metrics[incomplete[1]] and left.metrics[incomplete[0]] == left.metrics[incomplete[1]]:
        flags.append({"status": "regression", "finding": "Comparison run has incomplete turnarounds."})
    return {"status": "regression" if any(f["status"] == "regression" for f in flags) else "unknown" if any(f["status"] == "unknown" for f in flags) else "no_regression", "available_metrics": safety_keys, "findings": flags}


def determinism(left: Run, right: Run) -> dict[str, Any]:
    left_path, right_path = Path(left.source), Path(right.source)
    paths = [(left_path, right_path)]
    if left_path.suffix.lower() == ".json" and right_path.suffix.lower() == ".json":
        paths.extend((left_path.with_name(name), right_path.with_name(name)) for name in ("runs.csv", "summary.csv")
                     if left_path.with_name(name).exists() or right_path.with_name(name).exists())
    byte_identical = all(a.is_file() and b.is_file() and a.read_bytes() == b.read_bytes() for a, b in paths)
    changed = []
    keys = sorted(set(left.metrics) | set(right.metrics))
    for key in keys:
        a, b = left.metrics.get(key), right.metrics.get(key)
        if a != b:
            changed.append({"field": key, "left": a, "right": b})
    left_events = left.events or left.raw.get("runs", []) or left.raw.get("rows", [])
    right_events = right.events or right.raw.get("runs", []) or right.raw.get("rows", [])
    left_events, right_events = _stable_records(left_events), _stable_records(right_events)
    event_counts = {"left": len(left_events), "right": len(right_events)}
    changed_records = []
    for index, (a, b) in enumerate(zip(left_events, right_events)):
        if a != b:
            changed_records.append({"index": index, "left": a, "right": b})
            if len(changed_records) == 20: break
    if len(left_events) != len(right_events):
        changed_records.append({"index": min(len(left_events), len(right_events)), "left": "<end>" if len(left_events) <= len(right_events) else left_events[min(len(left_events), len(right_events))],
                                "right": "<end>" if len(right_events) <= len(left_events) else right_events[min(len(left_events), len(right_events))]})
    if byte_identical:
        status, message = "byte_identical", "Input files are byte-for-byte identical."
    elif left.metrics == right.metrics and left_events == right_events:
        status, message = "equivalent", "Structured metrics and ordered records match."
    else:
        status, message = "different", "Structured results differ; changed fields are listed."
    return {"status": status, "message": message, "changed_fields": changed,
            "changed_records": changed_records, "ordered_record_counts": event_counts}


def _stable_records(records: list[Any]) -> list[Any]:
    """Remove wall-clock measurements that are explicitly observational in RampLab exports."""
    volatile = {"execution_ms", "execution_seconds", "completed_at_utc"}
    if isinstance(records, list):
        return [_stable_records(record) if isinstance(record, (dict, list)) else record for record in records]
    if isinstance(records, dict):
        return {key: _stable_records(value) if isinstance(value, (dict, list)) else value
                for key, value in records.items() if key not in volatile}
    return records


def summarize(run: Run) -> dict[str, Any]:
    return {"schema_version": 1, "run": {"name": run.name, "source": run.source, "identity": run.identity,
            "metrics": run.metrics, "event_count": len(run.events), "event_counts": run.raw.get("event_counts", {}),
            "warnings": run.warnings}, "safety": {"status": "unknown" if not any(METRICS[k].category == "safety" for k in run.metrics if k in METRICS) else "metrics_available"}}


def _fmt(value: float, unit: str) -> str:
    return f"{value:,.3f} {unit}".strip()


def markdown(result: dict[str, Any], left: Run, right: Run | None = None, deterministic: dict[str, Any] | None = None) -> str:
    out = [f"# RampLab Experiment Analysis: {left.name}", "", "## Experiment overview", "",
           f"- Source: `{left.source}`", f"- Identity: `{json.dumps(left.identity, sort_keys=True)}`",
           f"- Metrics available: {len(left.metrics)}"]
    if right is None:
        out += ["", "## Run summary", "", "| KPI | Value |", "|---|---:|"]
        for key in sorted(left.metrics):
            metric = METRICS.get(key, Metric(key))
            out.append(f"| {metric.label} | {_fmt(left.metrics[key], metric.unit)} |")
        out += ["", "## Safety", ""]
        observed = [k for k in sorted(left.metrics) if k in METRICS and METRICS[k].category == "safety"]
        if observed:
            out.extend(f"- {METRICS[k].label}: {_fmt(left.metrics[k], METRICS[k].unit)}" for k in observed)
        else:
            out.append("No safety metrics were exported; safety status is **unknown**.")
        if not any(k in left.metrics for k in ("fleet_collisions", "surface_aircraft_aircraft_collisions", "surface_aircraft_ground_collisions")):
            out.append("Collision fields are absent, so this report does not declare the run safe.")
        out += ["", "## Conclusion", "", f"Summarized {len(left.metrics)} available numeric KPI(s).", ""]
    else:
        out += [f"", f"- Comparison: `{right.name}` (`{right.source}`)", "", "## KPI comparison", "",
                "| KPI | Baseline | Comparison | Change | Impact |", "|---|---:|---:|---:|---|"]
        for item in result["metrics"]:
            change = _fmt(item["difference"], item["unit"])
            if item["percent_difference"] is not None: change += f" ({item['percent_difference']:+.1f}%)"
            out.append(f"| {item['label']} | {_fmt(item['baseline'], item['unit'])} | {_fmt(item['comparison'], item['unit'])} | {change} | {item['impact']} |")
        out += ["", "## Operational impact", ""]
        if result["operational_impact"]:
            for item in result["operational_impact"]:
                out.append(f"- **{item['impact'].title()}: {item['label']}** — {item['difference']:+,.3f} {item['unit']}.")
        else: out.append("No shared operational KPI changed.")
        out += ["", "## Safety", "", f"Assessment: **{result['safety']['status']}**"]
        out.extend(f"- {item['finding']}" for item in result["safety"]["findings"])
        if not result["safety"]["findings"]: out.append("No shared safety metrics were available to identify a change.")
        if result["compatibility"]["mismatched_identity"]:
            out += ["", "## Compatibility notes", "", "Shared identity fields differ: " + ", ".join(f"`{key}`" for key in result["compatibility"]["mismatched_identity"]) + ". Interpret the comparison with this mismatch in mind."]
        out += ["", "## Disruption and recovery observations", "", "Available disruption, reroute, reassignment, completion, and failure fields are included in the KPI comparison. Recovery duration is not exported by the current schema.", "", "## Determinism", ""]
        if deterministic:
            out.append(f"- {deterministic['message']}")
            out.extend(f"- Changed `{x['field']}`: {x['left']} → {x['right']}" for x in deterministic["changed_fields"][:20])
            out.extend(f"- Ordered record {x['index'] + 1} differs: `{json.dumps(x['left'], sort_keys=True)}` → `{json.dumps(x['right'], sort_keys=True)}`" for x in deterministic["changed_records"][:10])
        out += ["", "## Conclusion", "", f"Compared {len(result['metrics'])} shared numeric KPI(s). {len(result['operational_impact'])} ranked change(s) are shown above.", ""]
    return "\n".join(out)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", help="JSON, CSV, or JSONL output from RampLab")
    parser.add_argument("comparison", nargs="?", help="second run for comparison or determinism check")
    parser.add_argument("--output", "-o", help="write Markdown report to this path")
    parser.add_argument("--json", dest="json_output", help="write machine-readable JSON report")
    parser.add_argument("--determinism", action="store_true", help="compare repeated runs for equivalent results")
    args = parser.parse_args(argv)
    try:
        left = load_run(args.input)
        right = load_run(args.comparison) if args.comparison else None
        comparison = compare(left, right) if right else None
        det = determinism(left, right) if right and args.determinism else None
        report = comparison if comparison is not None else summarize(left)
        if det is not None: report["determinism"] = det
        md = markdown(comparison, left, right, det) if right else markdown(report, left)
        if args.output: Path(args.output).write_text(md, encoding="utf-8")
        else: print(md)
        if args.json_output: Path(args.json_output).write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        return 0
    except (OSError, ValueError, json.JSONDecodeError, csv.Error) as error:
        print(f"experiment_analysis: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
