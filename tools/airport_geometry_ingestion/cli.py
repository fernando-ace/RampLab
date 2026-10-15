"""Command line interface for airport operational geometry datasets."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys
import zipfile

from . import SCHEMA_VERSION
from .core import (GeometryInputError, build_graph, canonical_bytes, compare_datasets,
                   convert_goal45_dataset, graph_diagnostics, import_source, markdown_readiness, readiness, render_svg,
                   validate_dataset)


def _write_json(path: Path, value) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(canonical_bytes(value))


def _load_dataset(path: Path) -> dict:
    try:
        value=json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise GeometryInputError(f"{path}: {exc}") from None
    if isinstance(value, dict) and value.get("schema_version") == "1.0" and "rejected_features" in value:
        return convert_goal45_dataset(path)
    if not isinstance(value, dict): raise GeometryInputError("canonical dataset must be a JSON object")
    return value


def _dataset_output(args) -> dict:
    path=Path(args.dataset)
    return _load_dataset(path)


def _parser() -> argparse.ArgumentParser:
    parser=argparse.ArgumentParser(prog="airport_geometry", description="Import, normalize, audit, compare, and bundle airport operational geometry.")
    subs=parser.add_subparsers(dest="command", required=True)
    p=subs.add_parser("inspect", help="summarize a canonical or mapped raw geometry dataset")
    p.add_argument("input", type=Path); p.add_argument("--mapping", type=Path)
    p=subs.add_parser("normalize", aliases=["convert"], help="normalize GeoJSON, CSV, KML/KMZ, or migrate Goal45 canonical JSON")
    p.add_argument("input", type=Path); p.add_argument("--mapping", type=Path); p.add_argument("--output", type=Path, required=True)
    p=subs.add_parser("validate", help="validate a canonical schema 2.0 dataset")
    p.add_argument("dataset", type=Path); p.add_argument("--report", type=Path)
    p=subs.add_parser("build-graph", help="build graph edges from supplied linework and snap supplied endpoints")
    p.add_argument("dataset", type=Path); p.add_argument("--output", type=Path, required=True); p.add_argument("--snap-tolerance-m", type=float, default=0.25)
    p=subs.add_parser("audit", help="write deterministic topology, readiness, and SVG inspection outputs")
    p.add_argument("dataset", type=Path); p.add_argument("--json", type=Path, required=True); p.add_argument("--markdown", type=Path, required=True); p.add_argument("--svg", type=Path, required=True); p.add_argument("--profile", choices=["production", "test_fixture"], default="production")
    p=subs.add_parser("readiness", help="evaluate the fail-closed readiness gate")
    p.add_argument("dataset", type=Path); p.add_argument("--json", type=Path, required=True); p.add_argument("--markdown", type=Path); p.add_argument("--profile", choices=["production", "test_fixture"], default="production"); p.add_argument("--require-ready", action="store_true")
    p=subs.add_parser("compare", help="compare two canonical geometry revisions")
    p.add_argument("before", type=Path); p.add_argument("after", type=Path); p.add_argument("--json", type=Path, required=True); p.add_argument("--markdown", type=Path, required=True); p.add_argument("--move-tolerance-m", type=float, default=0.01); p.add_argument("--profile", choices=["production","test_fixture"], default="production")
    p=subs.add_parser("bundle", help="write a deterministic ZIP, SHA-256 ledger, and ZIP integrity check")
    p.add_argument("directory", type=Path); p.add_argument("--output", type=Path, required=True)
    return parser


def main(argv: list[str] | None = None) -> int:
    args=_parser().parse_args(argv)
    try:
        if args.command in {"normalize", "convert"}:
            if args.mapping:
                dataset=import_source(args.input, args.mapping)
            else:
                candidate=json.loads(args.input.read_text(encoding="utf-8-sig"))
                dataset=candidate if isinstance(candidate,dict) and candidate.get("schema_version")==SCHEMA_VERSION else convert_goal45_dataset(args.input)
            errors=validate_dataset(dataset)
            if errors:
                _write_json(args.output, dataset)
                print(f"normalized output written, but canonical validation found {len(errors)} error(s): {args.output}")
                return 2
            _write_json(args.output, dataset); print(f"normalized {len(dataset['features'])} features to {args.output}"); return 0
        if args.command == "inspect":
            if args.mapping: dataset=import_source(args.input, args.mapping)
            else: dataset=_load_dataset(args.input)
            print(json.dumps({"airport_id":dataset.get("airport",{}).get("airport_id"),"schema_version":dataset.get("schema_version"),
                              "features":len(dataset.get("features",[])),"quarantined_features":len(dataset.get("quarantined_features",[])),
                              "feature_counts":{k:sum(1 for f in dataset.get("features",[]) if f.get("feature_type")==k) for k in sorted({f.get("feature_type") for f in dataset.get("features",[])})},
                              "validation_errors":len(validate_dataset(dataset))}, sort_keys=True, indent=2)); return 0
        if args.command == "validate":
            dataset=_dataset_output(args); errors=validate_dataset(dataset)
            if args.report: _write_json(args.report,{"schema_version":SCHEMA_VERSION,"valid":not errors,"findings":errors})
            print(f"{'VALID' if not errors else 'INVALID'}: {len(errors)} finding(s)")
            return 0 if not errors else 2
        if args.command == "build-graph":
            dataset=_dataset_output(args)
            errors=validate_dataset(dataset)
            if errors: raise GeometryInputError(f"refusing graph build for invalid dataset ({len(errors)} validation errors)")
            build_graph(dataset,args.snap_tolerance_m)
            _write_json(args.output,dataset); diagnostics=graph_diagnostics(dataset)
            print(f"built {len(dataset['operational_graph']['nodes'])} nodes and {len(dataset['operational_graph']['edges'])} edges; {len(diagnostics)} diagnostic(s)")
            return 0
        if args.command == "audit":
            dataset=_dataset_output(args); report=readiness(dataset,profile=args.profile)
            _write_json(args.json,report); args.markdown.parent.mkdir(parents=True,exist_ok=True); args.markdown.write_text(markdown_readiness(report),encoding="utf-8",newline="\n")
            args.svg.parent.mkdir(parents=True,exist_ok=True); args.svg.write_text(render_svg(dataset,report),encoding="utf-8",newline="\n")
            print(f"{report['decision']}: {sum(r['status']!='PASS' for r in report['requirements'])} requirement(s) need attention")
            return 0 if report["decision"]=="SIMULATOR_READY" else 2
        if args.command == "readiness":
            dataset=_dataset_output(args); report=readiness(dataset,profile=args.profile)
            _write_json(args.json,report)
            if args.markdown:
                args.markdown.parent.mkdir(parents=True,exist_ok=True); args.markdown.write_text(markdown_readiness(report),encoding="utf-8",newline="\n")
            print(f"{report['decision']} ({args.profile})")
            return 2 if args.require_ready and report["decision"]!="SIMULATOR_READY" else 0
        if args.command == "compare":
            before,after=_load_dataset(args.before),_load_dataset(args.after)
            report=compare_datasets(before,after,args.move_tolerance_m,profile=args.profile); _write_json(args.json,report)
            changed=sum(1 for row in report["changed_features"])
            text=(f"# Geometry revision comparison\n\nBefore: `{report['before_dataset_id']}`  \nAfter: `{report['after_dataset_id']}`\n\n"
                  f"- Added: {len(report['added_features'])}\n- Removed: {len(report['removed_features'])}\n- Changed: {changed}\n"
                  f"- Readiness: {report['readiness_change']['before']} → {report['readiness_change']['after']}\n")
            args.markdown.parent.mkdir(parents=True,exist_ok=True); args.markdown.write_text(text,encoding="utf-8",newline="\n"); return 0
        if args.command == "bundle":
            folder=args.directory.resolve(); output=args.output.resolve()
            if not folder.is_dir(): raise GeometryInputError(f"evidence directory does not exist: {folder}")
            if folder in output.parents: raise GeometryInputError("ZIP output must be outside the evidence directory")
            files=sorted(p for p in folder.rglob("*") if p.is_file() and p.name!="SHA256SUMS.txt")
            ledger="".join(f"{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.relative_to(folder).as_posix()}\n" for p in files)
            (folder/"SHA256SUMS.txt").write_text(ledger,encoding="utf-8",newline="\n")
            output.parent.mkdir(parents=True,exist_ok=True)
            with zipfile.ZipFile(output,"w",compression=zipfile.ZIP_DEFLATED,compresslevel=9) as archive:
                for path in sorted(p for p in folder.rglob("*") if p.is_file()):
                    info=zipfile.ZipInfo(path.relative_to(folder.parent).as_posix(),date_time=(1980,1,1,0,0,0))
                    info.compress_type=zipfile.ZIP_DEFLATED; info.external_attr=0o100644<<16
                    archive.writestr(info,path.read_bytes(),compress_type=zipfile.ZIP_DEFLATED,compresslevel=9)
            with zipfile.ZipFile(output) as archive:
                bad=archive.testzip()
                if bad: raise GeometryInputError(f"ZIP integrity failed at {bad}")
            expected={}
            for line in ledger.splitlines():
                digest,rel=line.split("  ",1); expected[rel]=digest
            mismatches=[rel for rel,digest in expected.items() if hashlib.sha256((folder/Path(rel)).read_bytes()).hexdigest()!=digest]
            if mismatches: raise GeometryInputError("checksum verification failed: "+", ".join(mismatches))
            with zipfile.ZipFile(output) as archive:
                archive_names=set(archive.namelist())
                missing=[f"{folder.name}/{rel}" for rel in expected if f"{folder.name}/{rel}" not in archive_names]
                archive_mismatches=[]
                for rel,digest in expected.items():
                    archive_name=f"{folder.name}/{rel}"
                    if archive_name in archive_names and hashlib.sha256(archive.read(archive_name)).hexdigest()!=digest:
                        archive_mismatches.append(rel)
                ledger_name=f"{folder.name}/SHA256SUMS.txt"
                if ledger_name not in archive_names or archive.read(ledger_name).decode("utf-8")!=ledger:
                    archive_mismatches.append("SHA256SUMS.txt")
                if missing or archive_mismatches: raise GeometryInputError("embedded ZIP checksum verification failed: "+", ".join(missing+archive_mismatches))
            digest=hashlib.sha256(output.read_bytes()).hexdigest()
            output.with_suffix(output.suffix+".sha256").write_text(f"{digest}  {output.name}\n",encoding="ascii",newline="\n")
            print(f"ZIP integrity PASS: {output}; SHA-256 {digest}"); return 0
    except (GeometryInputError, OSError, KeyError, TypeError, ValueError, json.JSONDecodeError) as exc:
        print(f"airport_geometry: error: {exc}",file=sys.stderr); return 2
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
