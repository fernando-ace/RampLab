from __future__ import annotations
import argparse
import json
import sys
from pathlib import Path
from .core import InputFailure, canonical_bytes, load_dataset

def main(argv=None):
    parser = argparse.ArgumentParser(prog="airport-data", description="Validate, normalize, build and inspect local airport operations data.")
    sub = parser.add_subparsers(dest="command", required=True)
    for cmd in ("validate", "normalize", "build"):
        p = sub.add_parser(cmd); p.add_argument("manifest", type=Path); p.add_argument("--report", type=Path, help="write deterministic validation JSON")
        if cmd in ("normalize", "build"): p.add_argument("--output", type=Path, required=True, help="canonical JSON output path")
    p = sub.add_parser("inspect"); p.add_argument("package", type=Path)
    args = parser.parse_args(argv)
    if args.command == "inspect":
        try: data = json.loads(args.package.read_text(encoding="utf-8"), parse_constant=lambda v: (_ for _ in ()).throw(ValueError(f"invalid numeric constant {v}")))
        except (OSError, ValueError) as e: print(f"inspect: {e}", file=sys.stderr); return 2
        if not isinstance(data, dict): print("inspect: package must be a JSON object", file=sys.stderr); return 2
        print(f"Airport: {data.get('airport', {}).get('name', data.get('airport', {}).get('airport_id', 'unknown'))} ({data.get('airport', {}).get('airport_id', 'unknown')})")
        print(f"Schema: {data.get('schema_version')} | Dataset: {data.get('dataset', {}).get('dataset_id', '')}")
        for label, key in (("Flights", "flights"), ("Aircraft", "aircraft"), ("Gates", "gates"), ("Equipment", "equipment"), ("Turnaround requirements", "turnaround_requirements"), ("Disruptions", "disruptions")):
            rows = data.get(key, []); print(f"{label}: {len(rows)}")
            if key == "flights": print(f"  arrivals: {sum(r.get('operation') == 'arrival' for r in rows)}; departures: {sum(r.get('operation') == 'departure' for r in rows)}")
        times = [r.get(field) for r in data.get("flights", []) for field in ("scheduled_time", "estimated_time", "actual_time") if r.get(field)]
        if times: print(f"Operation time range: {min(times)} .. {max(times)}")
        print(f"Validation: {data.get('validation', {}).get('status', 'unknown')} | Sources: {len(data.get('sources', []))}")
        for source in data.get("sources", []): print(f"  {source['filename']} sha256={source['sha256']}")
        return 0
    data, findings, counts = load_dataset(args.manifest)
    report = data["validation"] if data else {"schema_version": "1.0", "status": "error", "counts": counts, "findings": findings, "error_count": len(findings), "warning_count": 0, "info_count": 0}
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True); args.report.write_text(json.dumps(report, sort_keys=True, indent=2) + "\n", encoding="utf-8")
    for table, count in sorted(counts.items()): print(f"{table}: {count}")
    for f in findings: print(f"{f['severity'].upper()} {f['code']} {f['source']}" + (f" row {f['record']}" if f['record'] else "") + (f" field {f['field']}" if f['field'] else "") + f": {f['message']}")
    if report["status"] == "error": print(f"Validation failed: {report['error_count']} error(s)", file=sys.stderr); return 1
    if args.command in ("normalize", "build"):
        args.output.parent.mkdir(parents=True, exist_ok=True); args.output.write_bytes(canonical_bytes(data))
        print(f"Wrote canonical package: {args.output}")
    print(f"Validation {report['status']}")
    return 0

if __name__ == "__main__": raise SystemExit(main())
