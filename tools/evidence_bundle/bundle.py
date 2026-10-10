"""Build a portable engineering evidence bundle from existing RampLab experiment outputs."""
from __future__ import annotations
import argparse, csv, hashlib, html, json, shutil, sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "experiment_analysis"))
import analyze as goal19
FORMAT_VERSION = "1.0"
ARTIFACTS = ("experiment.json", "summary.json", "metrics.json", "runs.csv", "summary.csv", "aircraft.csv", "events.jsonl", "interventions.json", "unreal-live.png", "unreal-live.log")
COLLISION_KEYS = ("fleet_collisions", "surface_aircraft_aircraft_collisions", "surface_aircraft_ground_collisions")
SEPARATION_KEYS = ("fleet_minimum_separation_m", "minimum_aircraft_separation_m", "minimum_aircraft_ground_separation_m")
FAILURE_KEYS = ("surface_safe_failures", "failed_or_timed_out_turnarounds", "fleet_requests_failed", "unresolved_service_requests")

def digest(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""): h.update(chunk)
    return h.hexdigest()

def json_object(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict): raise ValueError(f"{path.name} must contain a JSON object")
    return value

def csv_rows(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8-sig") as f:
        reader = csv.DictReader(f, strict=True)
        if not reader.fieldnames: raise ValueError(f"{path.name} has no CSV header")
        return list(reader)

def number(value: Any) -> float | None:
    if isinstance(value, bool): return None
    try:
        n = float(value)
        return n if n == n and abs(n) != float("inf") else None
    except (TypeError, ValueError): return None

def inspect(label: str, directory: Path) -> dict[str, Any]:
    directory = directory.expanduser().resolve()
    if not directory.is_dir(): raise ValueError(f"{label} must be an existing output directory: {directory}")
    files = {n: directory / n for n in ARTIFACTS if (directory / n).is_file()}
    primary = next((files[n] for n in ("experiment.json", "summary.json", "metrics.json") if n in files), None)
    if primary is None and not ({"runs.csv", "summary.csv"} & files.keys()):
        raise ValueError(f"{label} has no experiment JSON, runs.csv, or summary.csv")
    raw = json_object(primary) if primary else {}
    run_rows = csv_rows(files["runs.csv"]) if "runs.csv" in files else []
    aircraft_rows = csv_rows(files["aircraft.csv"]) if "aircraft.csv" in files else []
    if "summary.csv" in files: csv_rows(files["summary.csv"])
    events = []
    if "events.jsonl" in files:
        for i, line in enumerate(files["events.jsonl"].read_text(encoding="utf-8").splitlines()):
            if line.strip():
                event = json.loads(line)
                if not isinstance(event, dict): raise ValueError(f"events.jsonl line {i+1} must be an object")
                events.append((i, event))
    parsed = goal19.load_run(primary or files.get("runs.csv", files.get("summary.csv")))
    metrics = parsed.metrics
    checks = []
    observed = len(run_rows) if run_rows else (len(raw["runs"]) if isinstance(raw.get("runs"), list) else None)
    declared = raw.get("run_count")
    json_count = len(raw["runs"]) if isinstance(raw.get("runs"), list) else None
    contradictory_counts = bool(run_rows and json_count is not None and len(run_rows) != json_count)
    if contradictory_counts:
        checks.append({"check":"run_count_consistency","status":"FAIL","detail":f"JSON contains {json_count} runs; runs.csv contains {len(run_rows)} rows."})
    elif declared is not None and observed is not None and number(declared) != observed:
        checks.append({"check":"run_count_consistency","status":"FAIL","detail":f"Declared {declared}; found {observed} rows."})
    else:
        checks.append({"check":"run_count_consistency","status":"PASS" if observed is not None else "INSUFFICIENT DATA","detail":f"Observed {observed} run rows." if observed is not None else "Run count unavailable."})
    js = sorted({str(r["seed"]) for r in raw.get("runs",[]) if isinstance(r,dict) and r.get("seed") is not None})
    cs = sorted({r["seed"] for r in run_rows if r.get("seed")})
    declared_seeds = raw.get("seeds",{}).get("values",[]) if isinstance(raw.get("seeds"),dict) else []
    expected = sorted(map(str,declared_seeds))
    mismatch = bool((js and cs and js != cs) or (expected and (js or cs) and expected != (js or cs)))
    checks.append({"check":"seed_consistency","status":"FAIL" if mismatch else "PASS" if js or cs or expected else "INSUFFICIENT DATA","detail":f"JSON={js or 'none'}, CSV={cs or 'none'}, declared={expected or 'none'}."})
    aircraft_total = metrics.get("aircraft_count",metrics.get("surface_total_aircraft"))
    if aircraft_rows and aircraft_total is not None:
        checks.append({"check":"aircraft_totals","status":"PASS" if len(aircraft_rows)==aircraft_total else "FAIL","detail":f"Aircraft rows {len(aircraft_rows)}; KPI {aircraft_total:g}."})
    else: checks.append({"check":"aircraft_totals","status":"INSUFFICIENT DATA","detail":"Aircraft rows or total KPI unavailable."})
    collisions = [k for k in COLLISION_KEYS if k in metrics]
    checks.append({"check":"collision_evidence","status":"WARNING" if not collisions else "PASS" if all(metrics[k]==0 for k in collisions) else "FAIL","detail":"Collision evidence unavailable." if not collisions else "Collision KPI(s): "+", ".join(collisions)})
    separated = any(k in metrics for k in SEPARATION_KEYS)
    checks.append({"check":"minimum_separation_evidence","status":"PASS" if separated else "INSUFFICIENT DATA","detail":"Minimum separation KPI present." if separated else "Minimum separation unavailable."})
    checks.append({"check":"determinism_evidence","status":"INSUFFICIENT DATA","detail":"One experiment does not establish repeat-run determinism."})
    checks.append({"check":"event_file_parseability","status":"PASS" if "events.jsonl" in files else "INSUFFICIENT DATA","detail":f"Parsed {len(events)} ordered events." if "events.jsonl" in files else "No event history supplied."})
    if primary: checks.append({"check":"json_readability","status":"PASS","detail":f"Parsed {primary.name}."})
    json_scenario = raw.get("source_scenario")
    csv_scenarios = sorted({r.get("scenario") for r in run_rows if r.get("scenario")})
    if json_scenario and csv_scenarios:
        source_name = Path(str(json_scenario).replace("\\", "/")).stem
        scenario_ok = all(s == source_name or str(json_scenario).replace("\\", "/").endswith("/" + str(s) + ".yaml") for s in csv_scenarios)
        checks.append({"check":"scenario_identity","status":"PASS" if scenario_ok else "FAIL","detail":f"JSON source scenario {json_scenario}; CSV scenario values {csv_scenarios}."})
    safety = ("FAIL" if any(metrics.get(k,0)>0 for k in COLLISION_KEYS+FAILURE_KEYS)
              else "INSUFFICIENT DATA" if not collisions or not separated else "NO RECORDED COLLISIONS")
    seeds = declared_seeds or sorted({int(s) if s.isdigit() else s for s in (js or cs)})
    completion = None
    completed, total = metrics.get("completed_turnarounds"), metrics.get("total_turnarounds")
    if completed is not None and total is not None:
        completion = "COMPLETE" if completed == total else "INCOMPLETE"
    return {"label":label,"source":directory,"files":files,"raw":raw,"name":str(raw.get("experiment_name",directory.name)),
            "scenario":raw.get("source_scenario",run_rows[0].get("scenario") if run_rows else None),"seeds":seeds,
            "metrics":metrics,"events":events,"run_count":observed,"findings":checks,"safety":safety,"completion":completion}

def overall(checks: list[dict[str,str]]) -> str:
    states={c["status"] for c in checks}
    return "FAIL" if "FAIL" in states else "WARNING" if "WARNING" in states else "INSUFFICIENT DATA" if "INSUFFICIENT DATA" in states else "PASS"

def make_kpis(runs: list[dict[str,Any]]) -> dict[str,Any]:
    keys=sorted(set().union(*(r["metrics"].keys() for r in runs)))
    result={"experiments":{r["label"]:{k:r["metrics"][k] for k in keys if k in r["metrics"]} for r in runs}}
    if len(runs)==2:
        changes={}
        for k in keys:
            a,b=runs[0]["metrics"].get(k),runs[1]["metrics"].get(k)
            if a is not None and b is not None:
                delta=b-a
                changes[k]={"baseline":a,"comparison":b,"absolute_change":delta,"percent_change":delta/abs(a)*100 if a else None}
        result["comparison"]={"baseline":runs[0]["label"],"comparison":runs[1]["label"],"metrics":changes}
    return result

def make_timeline(runs: list[dict[str,Any]]) -> list[dict[str,Any]]:
    return [{"experiment":r["label"],"source":f"evidence/{r['label']}/events.jsonl","line":i+1,
             "sequence":e.get("sequence"),"time_seconds":e.get("time_seconds"),"type":e.get("type",e.get("event","unknown")),"event":e}
            for r in runs for i,e in r["events"]]

def render_readme(runs,kpis,checks,timeline,det) -> str:
    lines=["# RampLab Engineering Evidence Bundle","","## Experiments","","| Name | Scenario | Seeds | Runs | Completion | Safety |","|---|---|---:|---:|---|---|"]
    for r in runs: lines.append(f"| {r['name']} | {r['scenario'] or 'Unavailable'} | {', '.join(map(str,r['seeds'])) or 'Unavailable'} | {r['run_count'] if r['run_count'] is not None else 'Unavailable'} | {r['completion'] or 'Unavailable'} | {r['safety']} |")
    lines += ["","## KPI summary","","| KPI | "+" | ".join(r["label"] for r in runs)+(" | Absolute change | Percent change |" if len(runs)==2 else " |"),"|---|"+"---|"*(len(runs)+(2 if len(runs)==2 else 0))]
    keys=sorted(set().union(*(r["metrics"].keys() for r in runs)))
    for k in keys:
        definition=goal19.METRICS.get(k); label,unit=(definition.label,definition.unit) if definition else (k,"")
        vals=[r["metrics"].get(k) for r in runs]
        fmt=lambda v:"Unavailable" if v is None else f"{v:,.3f} {unit}".strip()
        tail=""
        if len(runs)==2 and k in kpis["comparison"]["metrics"]:
            c=kpis["comparison"]["metrics"][k]
            tail=f" | {fmt(c['absolute_change'])} | {c['percent_change']:.2f}%" if c["percent_change"] is not None else f" | {fmt(c['absolute_change'])} | N/A (zero baseline)"
        lines.append(f"| {label} ({k}) | "+" | ".join(map(fmt,vals))+tail+" |")
    lines += ["","## Safety and determinism","","Safety: "+"; ".join(f"{r['label']}: {r['safety']}" for r in runs),"",f"Determinism: {det['status']} — {det['detail']}","","## Validation","",f"Overall: **{overall(checks)}**","","| Check | Status | Detail |","|---|---|---|"]
    lines += [f"| {c['check']} | {c['status']} | {c['detail']} |" for c in checks]
    disruptions = [e for e in timeline if any(word in str(e["type"]).lower() for word in
        ("operatorintervention", "outage", "unavailable", "rerout", "reassign", "timeout", "failure", "closure", "roadclosed", "replan"))]
    lines += ["","## Recorded disruptions",""]
    lines += [f"- {e['experiment']} event line {e['line']} (sequence {e['sequence'] if e['sequence'] is not None else 'unavailable'}): {e['type']}" for e in disruptions] or ["No disruption event types were recorded. Operational reroutes and reassignments remain listed in the KPI table."]
    lines += ["","## Event timeline",""]
    lines += [f"- {e['experiment']} line {e['line']} (sequence {e['sequence'] if e['sequence'] is not None else 'unavailable'}, t={e['time_seconds'] if e['time_seconds'] is not None else 'unavailable'} s): {e['type']}" for e in timeline] or ["No event history supplied."]
    lines += ["","## Evidence files","","Source artifacts are copied byte-for-byte under evidence/. SHA-256 hashes are in manifest.json. See kpis.json and validation.json.","","## Limitations","","Missing values remain unavailable. Collision-free values describe recorded observations only. Determinism is not established by one export or by comparing different scenarios.",""]
    return "\n".join(lines)

def render_html(runs,checks,det,copied,kpis) -> str:
    esc=html.escape
    rows="".join(f"<tr><td>{esc(r['name'])}</td><td>{esc(str(r['scenario'] or 'Unavailable'))}</td><td>{esc(', '.join(map(str,r['seeds'])) or 'Unavailable')}</td><td>{esc(str(r['run_count'] if r['run_count'] is not None else 'Unavailable'))}</td><td>{esc(r['completion'] or 'Unavailable')}</td><td>{esc(r['safety'])}</td></tr>" for r in runs)
    checkrows="".join(f"<tr><td>{esc(c['check'])}</td><td>{esc(c['status'])}</td><td>{esc(c['detail'])}</td></tr>" for c in checks)
    evidence="".join(f"<li><a href='{esc(f['bundle_path'],quote=True)}'>{esc(f['bundle_path'])}</a> — SHA-256 <code>{f['sha256']}</code></li>" for f in copied)
    timeline=[{"experiment":r["label"],"line":i+1,"sequence":e.get("sequence"),"type":e.get("type",e.get("event","unknown"))} for r in runs for i,e in r["events"]]
    disruptions=[e for e in timeline if any(word in str(e["type"]).lower() for word in ("outage","unavailable","rerout","reassign","timeout","failure","closure","replan"))]
    disruption_html="".join(f"<li>{esc(e['experiment'])}, line {e['line']}, sequence {esc(str(e['sequence'] or 'unavailable'))}: {esc(str(e['type']))}</li>" for e in disruptions) or "<li>No disruption event types recorded.</li>"
    keys=sorted(set().union(*(r["metrics"].keys() for r in runs)))
    kp=[]
    for k in keys:
        d=goal19.METRICS.get(k); values=[r["metrics"].get(k) for r in runs]
        label=(d.label + (f" ({d.unit})" if d.unit else "")) if d else k
        display=lambda v: "Unavailable" if v is None else f"{v:,.3f}"
        row="<tr><td>"+esc(label)+"</td>"+"".join(f"<td>{esc(display(v))}</td>" for v in values)
        if len(runs)==2:
            change=kpis["comparison"]["metrics"].get(k)
            if change:
                percent="N/A (zero baseline)" if change["percent_change"] is None else f"{change['percent_change']:.2f}%"
                delta_display=f"{change['absolute_change']:,.3f}"
                row+=f"<td>{esc(delta_display)}</td><td>{esc(percent)}</td>"
            else: row+="<td>Unavailable</td><td>Unavailable</td>"
        kp.append(row+"</tr>")
    head="".join(f"<th>{esc(r['label'])}</th>" for r in runs)+("<th>Absolute change</th><th>Percent change</th>" if len(runs)==2 else "")
    return f"""<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width"><title>RampLab Evidence Bundle</title><style>body{{font:16px/1.5 system-ui,sans-serif;max-width:1100px;margin:2rem auto;padding:0 1rem;color:#17212b}}h1,h2{{color:#123c5a}}table{{border-collapse:collapse;width:100%;margin:1rem 0}}th,td{{border:1px solid #ccd5dc;padding:.55rem;text-align:left}}th{{background:#eef3f6}}code{{overflow-wrap:anywhere}}.status{{padding:.8rem;background:#eef3f6;border-left:4px solid #3278a6}}</style><body><h1>RampLab Engineering Evidence Bundle</h1><p class="status">Validation: <b>{overall(checks)}</b> · Determinism: <b>{det['status']}</b></p><h2>Experiment identity and outcomes</h2><table><tr><th>Name</th><th>Scenario</th><th>Seeds</th><th>Runs</th><th>Completion</th><th>Safety</th></tr>{rows}</table><h2>KPI summary</h2><table><tr><th>KPI</th>{head}</tr>{''.join(kp)}</table><p><a href="kpis.json">Machine-readable KPI changes</a></p><h2>Recorded disruptions</h2><ul>{disruption_html}</ul><h2>Validation status</h2><table><tr><th>Check</th><th>Status</th><th>Detail</th></tr>{checkrows}</table><h2>Artifact inventory and provenance</h2><ul>{evidence}</ul><p>Source files are copied byte-for-byte. No external resources are required.</p></body></html>"""

def build(inputs: list[tuple[str,Path]], output: Path,
          determinism_report: Path | None = None) -> dict[str,Any]:
    if len(inputs) not in (1,2): raise ValueError("Provide one experiment or a control/disruption pair")
    runs=[inspect(label,path) for label,path in inputs]
    checks=[c|{"experiment":r["label"]} for r in runs for c in r["findings"]]
    det={"status":"INSUFFICIENT DATA","detail":"No independent repeat-run pair was supplied; scenario comparison is not determinism evidence."}
    determinism_source = None
    if determinism_report is not None:
        determinism_source = determinism_report.expanduser().resolve()
        source = json.loads(determinism_source.read_text(encoding="utf-8"))
        results = source.get("determinism") if isinstance(source, dict) else None
        statuses = [item.get("status") for item in results.values() if isinstance(item, dict)] if isinstance(results, dict) else []
        passed = len(statuses) >= 4 and all(status in {"equivalent", "byte_identical"} for status in statuses)
        det = {"status":"PASS" if passed else "FAIL",
               "detail":f"Repeated-run report contains {len(statuses)} checks: " + ", ".join(statuses)}
        for check in checks:
            if check["check"] == "determinism_evidence":
                check["status"] = det["status"]
                check["detail"] = f"Independent repeat-run evidence supplied; see determinism.json. {det['detail']}"
        checks.append({"check":"repeat_run_determinism","status":det["status"],"detail":det["detail"],"experiment":"comparison"})
    if len(runs)==2:
        for r in runs:
            if r["scenario"] is None: checks.append({"check":"scenario_identity","status":"INSUFFICIENT DATA","detail":f"{r['label']} scenario unavailable.","experiment":r["label"]})
        a,b=runs[0]["scenario"],runs[1]["scenario"]
        if a is not None and b is not None and a!=b: checks.append({"check":"scenario_identity","status":"WARNING","detail":f"Scenarios differ: {a} vs {b}.","experiment":"comparison"})
    output=output.expanduser().resolve()
    if output.exists() and any(output.iterdir()): raise ValueError(f"Output directory must be empty: {output}")
    output.mkdir(parents=True,exist_ok=True)
    copied,missing,recognized=[],[],{}
    for r in runs:
        dest=output/"evidence"/r["label"]; dest.mkdir(parents=True,exist_ok=True); recognized[r["label"]]=[]
        for name in ARTIFACTS:
            src=r["files"].get(name)
            if src is None: missing.append({"experiment":r["label"],"artifact":name}); continue
            target=dest/name; shutil.copyfile(src,target)
            copied.append({"experiment":r["label"],"source_path":str(src),"bundle_path":target.relative_to(output).as_posix(),"sha256":digest(target),"size_bytes":target.stat().st_size})
            recognized[r["label"]].append(name)
    kpis,timeline=make_kpis(runs),make_timeline(runs)
    if determinism_source is not None:
        det_target = output / "determinism.json"
        shutil.copyfile(determinism_source, det_target)
        copied.append({"experiment":"comparison","source_path":str(determinism_source),"bundle_path":"determinism.json",
                       "sha256":digest(det_target),"size_bytes":det_target.stat().st_size})
    validation={"status":overall(checks),"checks":checks,"hash_verification":{"status":"PASS" if all(digest(output/f["bundle_path"])==f["sha256"] for f in copied) else "FAIL","files_checked":len(copied)},"missing_evidence":missing}
    manifest={"format_version":FORMAT_VERSION,"creation_metadata":{"created_at_utc":datetime.now(timezone.utc).isoformat(timespec="seconds")},
      "inputs":[{"name":r["name"],"label":r["label"],"scenario":r["scenario"],"seeds":r["seeds"],"run_count":r["run_count"],"completion_status":r["completion"],"safety_status":r["safety"]} for r in runs],
      "input_files":copied,"recognized_artifact_types":recognized,"missing_optional_artifacts":missing,"analysis_files":["kpis.json","validation.json","timeline.json"],
      "generated_outputs":["README.md","manifest.json","index.html","kpis.json","validation.json","timeline.json"] + (["determinism.json"] if determinism_source else []),
      "safety_status":{r["label"]:r["safety"] for r in runs},"determinism_status":det["status"],
      "warnings":[c["detail"] for c in checks if c["status"] in ("WARNING","FAIL")]}
    payload={"README.md":render_readme(runs,kpis,checks,timeline,det),"kpis.json":json.dumps(kpis,indent=2,sort_keys=True,ensure_ascii=False)+"\n",
      "validation.json":json.dumps(validation,indent=2,sort_keys=True,ensure_ascii=False)+"\n","timeline.json":json.dumps(timeline,indent=2,sort_keys=True,ensure_ascii=False)+"\n",
      "index.html":render_html(runs,checks,det,copied,kpis),"manifest.json":json.dumps(manifest,indent=2,sort_keys=True,ensure_ascii=False)+"\n"}
    for name,content in payload.items(): (output/name).write_text(content,encoding="utf-8",newline="\n")
    return {"output":output,"manifest":manifest,"validation":validation}

def main(argv: list[str] | None=None) -> int:
    parser=argparse.ArgumentParser(description=__doc__)
    group=parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--input",type=Path,help="Single existing experiment output directory")
    group.add_argument("--control",type=Path,help="Control directory (requires --disruption)")
    parser.add_argument("--disruption",type=Path,help="Disruption output directory")
    parser.add_argument("--output",required=True,type=Path,help="New or empty bundle directory")
    parser.add_argument("--determinism-report",type=Path,help="Independent repeat-run analysis JSON to include and validate")
    args=parser.parse_args(argv)
    if bool(args.control)!=bool(args.disruption): parser.error("--control and --disruption must be supplied together")
    try:
        sources=[("experiment",args.input)] if args.input else [("control",args.control),("disruption",args.disruption)]
        result=build(sources,args.output,args.determinism_report)
        print(f"Bundle written to {result['output']}")
        print(f"Validation: {result['validation']['status']}; determinism: {result['manifest']['determinism_status']}")
        return 0
    except (OSError,ValueError,json.JSONDecodeError,csv.Error) as exc:
        print(f"evidence-bundle: {exc}",file=sys.stderr); return 2

if __name__=="__main__": raise SystemExit(main())
