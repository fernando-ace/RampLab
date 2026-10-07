from __future__ import annotations
import csv, json, shutil, tempfile, unittest
from html.parser import HTMLParser
from pathlib import Path
from .bundle import build, digest

ROOT=Path(__file__).resolve().parents[2]
FIX=ROOT/"tools"/"ops_dashboard"/"fixtures"

class LocalPage(HTMLParser):
    def __init__(self):
        super().__init__(); self.links=[]; self.external=[]
    def handle_starttag(self,tag,attrs):
        attrs=dict(attrs)
        if tag=="a": self.links.append(attrs.get("href",""))
        if tag=="script" and attrs.get("src","").startswith(("http:","https:")): self.external.append(attrs["src"])
        if tag=="link" and attrs.get("href","").startswith(("http:","https:")): self.external.append(attrs["href"])

class EvidenceBundleTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory()
        self.root=Path(self.temp.name)
        self.control=self.root/"control"; self.disruption=self.root/"disruption"
        shutil.copytree(FIX/"control",self.control); shutil.copytree(FIX/"disruption",self.disruption)
        self.out=self.root/"bundle"
    def tearDown(self): self.temp.cleanup()
    def test_valid_single_experiment_and_goal19_reuse(self):
        result=build([("experiment",self.control)],self.out)
        self.assertEqual(result["manifest"]["format_version"],"1.0")
        self.assertEqual(result["validation"]["hash_verification"]["status"],"PASS")
        self.assertEqual(json.loads((self.out/"kpis.json").read_text())["experiments"]["experiment"]["surface_taxi_distance_m"],672.0)
        self.assertEqual(digest(self.out/"evidence"/"experiment"/"runs.csv"),next(f["sha256"] for f in result["manifest"]["input_files"] if f["bundle_path"].endswith("runs.csv")))
    def test_pair_kpis_timeline_and_standalone_html(self):
        result=build([("control",self.control),("disruption",self.disruption)],self.out)
        kpis=json.loads((self.out/"kpis.json").read_text())
        self.assertEqual(kpis["comparison"]["metrics"]["surface_taxi_distance_m"]["absolute_change"],263)
        events=json.loads((self.out/"timeline.json").read_text())
        self.assertEqual([e["line"] for e in events if e["experiment"]=="control"],[1,2,3,4,5])
        page=(self.out/"index.html").read_text()
        self.assertIn("No external resources",page); self.assertNotIn("https://",page)
        self.assertIn("<th>Absolute change</th>",page); self.assertIn("<th>Percent change</th>",page)
        parsed=LocalPage(); parsed.feed(page)
        self.assertFalse(parsed.external)
        self.assertTrue(all((self.out/link).is_file() for link in parsed.links))
        self.assertEqual(result["manifest"]["determinism_status"],"INSUFFICIENT DATA")
    def test_missing_optional_files_and_safety(self):
        (self.control/"aircraft.csv").unlink(); (self.control/"events.jsonl").unlink()
        p=self.control/"experiment.json"; data=json.loads(p.read_text())
        for run in data["runs"]:
            for key in ("fleet_collisions","surface_aircraft_aircraft_collisions","surface_aircraft_ground_collisions","fleet_minimum_separation_m","minimum_aircraft_separation_m","minimum_aircraft_ground_separation_m"): run.pop(key,None)
        p.write_text(json.dumps(data)); (self.control/"runs.csv").write_text("ordinal,seed,scenario\n1,42,turnaround_flight_bank\n")
        result=build([("experiment",self.control)],self.out)
        self.assertEqual(result["manifest"]["safety_status"]["experiment"],"INSUFFICIENT DATA")
        self.assertIn({"experiment":"experiment","artifact":"events.jsonl"},result["manifest"]["missing_optional_artifacts"])
    def test_malformed_json_rejected(self):
        (self.control/"experiment.json").write_text("{bad")
        with self.assertRaises(json.JSONDecodeError): build([("experiment",self.control)],self.out)
    def test_malformed_csv_rejected(self):
        (self.control/"runs.csv").write_text('"unterminated')
        with self.assertRaises(csv.Error): build([("experiment",self.control)],self.out)
    def test_seed_disagreement_detected(self):
        with (self.control/"runs.csv").open(newline="") as f: rows=list(csv.DictReader(f)); fields=rows[0].keys()
        rows[0]["seed"]="99"
        with (self.control/"runs.csv").open("w",newline="") as f:
            w=csv.DictWriter(f,fieldnames=fields); w.writeheader(); w.writerows(rows)
        result=build([("experiment",self.control)],self.out)
        self.assertEqual(next(c["status"] for c in result["validation"]["checks"] if c["check"]=="seed_consistency"),"FAIL")
    def test_run_count_disagreement_detected(self):
        p=self.control/"experiment.json"; data=json.loads(p.read_text()); data["run_count"]=9; p.write_text(json.dumps(data))
        result=build([("experiment",self.control)],self.out)
        self.assertEqual(next(c["status"] for c in result["validation"]["checks"] if c["check"]=="run_count_consistency"),"FAIL")
    def test_scenario_disagreement_detected_within_artifacts(self):
        p=self.control/"runs.csv"
        text=p.read_text().replace("turnaround_flight_bank","unexpected_scenario")
        p.write_text(text)
        result=build([("experiment",self.control)],self.out)
        self.assertEqual(next(c["status"] for c in result["validation"]["checks"] if c["check"]=="scenario_identity"),"FAIL")
    def test_generated_content_stable_except_creation_metadata(self):
        build([("experiment",self.control)],self.out)
        names=("README.md","kpis.json","validation.json","timeline.json","index.html")
        first={n:(self.out/n).read_bytes() for n in names}; second=self.root/"bundle2"; build([("experiment",self.control)],second)
        self.assertEqual(first,{n:(second/n).read_bytes() for n in names})
    def test_missing_collision_fields_never_pass(self):
        p=self.control/"experiment.json"; data=json.loads(p.read_text())
        for run in data["runs"]:
            for key in ("fleet_collisions","surface_aircraft_aircraft_collisions","surface_aircraft_ground_collisions"): run.pop(key,None)
        p.write_text(json.dumps(data))
        (self.control/"runs.csv").unlink()
        result=build([("experiment",self.control)],self.out)
        self.assertEqual(result["manifest"]["safety_status"]["experiment"],"INSUFFICIENT DATA")

if __name__=="__main__": unittest.main()
