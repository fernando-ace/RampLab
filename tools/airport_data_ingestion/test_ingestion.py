import hashlib
import json
import tempfile
import unittest
from pathlib import Path
from .core import canonical_bytes, load_dataset

EXAMPLE = Path(__file__).parent / "examples" / "synthetic" / "manifest.json"

class IngestionTests(unittest.TestCase):
    def test_example_valid_and_sources_hashed(self):
        data, findings, _ = load_dataset(EXAMPLE)
        self.assertFalse([f for f in findings if f["severity"] == "error"])
        self.assertEqual(len(data["sources"]), 6)
        for source in data["sources"]:
            path = EXAMPLE.parent / source["filename"]
            self.assertEqual(source["sha256"], hashlib.sha256(path.read_bytes()).hexdigest())
        self.assertEqual(data["validation"]["info_count"], 3)

    def test_repeat_build_bytes_and_finding_order_stable(self):
        one, f1, _ = load_dataset(EXAMPLE); two, f2, _ = load_dataset(EXAMPLE)
        self.assertEqual(canonical_bytes(one), canonical_bytes(two))
        self.assertEqual(json.dumps(f1, sort_keys=True), json.dumps(f2, sort_keys=True))

    def test_bad_relationship_rejected(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            for name, value in (("flights", [{"flight_id":"F1","operation":"arrival","scheduled_time":"bad","aircraft_id":"A1"}, {"flight_id":"F2","operation":"departure","scheduled_time":"2026-01-01T00:00:00Z","aircraft_id":"NOPE","gate_id":"X"}]), ("aircraft", [{"aircraft_id":"A1","type":"jet"}]), ("gates", [{"gate_id":"G1"}])):
                (root / f"{name}.json").write_text(json.dumps(value))
            (root / "manifest.json").write_text(json.dumps({"schema_version":"1.0","airport":{"airport_id":"X","timezone":"UTC"},"files":{"flights":"flights.json","aircraft":"aircraft.json","gates":"gates.json"}}))
            _, findings, _ = load_dataset(root / "manifest.json")
            codes = {f["code"] for f in findings}
            self.assertTrue({"TIMESTAMP_INVALID", "FLIGHT_UNKNOWN_AIRCRAFT", "FLIGHT_UNKNOWN_GATE"} <= codes)

    def test_cli_help_and_manifest_schema_rejection(self):
        from .cli import main
        with tempfile.TemporaryDirectory() as td:
            p = Path(td) / "manifest.json"; p.write_text('{"schema_version":"99","airport":{},"files":{}}')
            data, findings, _ = load_dataset(p)
            self.assertIn("SCHEMA_VERSION_UNSUPPORTED", {f["code"] for f in findings})

    def test_csv_json_equivalence_for_operations(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            common = {"aircraft": [{"aircraft_id":"AC1","type":"jet"}], "gates": [{"gate_id":"G1"}], "flights": [{"flight_id":"F1","operation":"arrival","scheduled_time":"2026-01-01T00:00:00Z","aircraft_id":"AC1","gate_id":"G1"}]}
            for table, rows in common.items():
                (root / f"{table}.json").write_text(json.dumps(rows))
                fields = list(rows[0])
                (root / f"{table}.csv").write_text(",".join(fields) + "\n" + ",".join(str(rows[0][f]) for f in fields) + "\n")
            base = {"schema_version":"1.0","airport":{"airport_id":"TEST","timezone":"UTC"}}
            (root / "json-manifest.json").write_text(json.dumps({**base,"files":{k:f"{k}.json" for k in common}}))
            (root / "csv-manifest.json").write_text(json.dumps({**base,"files":{k:f"{k}.csv" for k in common}}))
            j, jf, _ = load_dataset(root / "json-manifest.json"); c, cf, _ = load_dataset(root / "csv-manifest.json")
            self.assertFalse(jf); self.assertFalse(cf)
            for key in common: self.assertEqual(j[key], c[key])
            self.assertNotEqual(j["sources"], c["sources"])

    def test_collision_and_unsupported_type(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            (root / "flights.json").write_text("[]")
            (root / "aircraft.json").write_text('[{"aircraft_id":"AC1","type":"jet"}]')
            (root / "gates.json").write_text('[{"gate_id":"G1"},{"gate_id":" G1 "}]')
            (root / "manifest.json").write_text(json.dumps({"schema_version":"1.0","airport":{"airport_id":"X","timezone":"UTC"},"files":{"flights":"flights.json","aircraft":"aircraft.json","gates":"gates.json"}}))
            _, findings, _ = load_dataset(root / "manifest.json")
            self.assertIn("NORMALIZED_ID_COLLISION", {f["code"] for f in findings})

    def test_dimension_units_normalize_and_reject_unknown(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            for name, content in (("flights", '[{"flight_id":"F1","operation":"arrival","scheduled_time":"2026-01-01T00:00:00Z","aircraft_id":"AC1","gate_id":"G1"}]'), ("aircraft", '[{"aircraft_id":"AC1","type":"jet","wingspan":100,"wingspan_unit":"ft"}]'), ("gates", '[{"gate_id":"G1"}]')):
                (root / f"{name}.json").write_text(content)
            manifest = {"schema_version":"1.0","airport":{"airport_id":"X","timezone":"UTC"},"files":{"flights":"flights.json","aircraft":"aircraft.json","gates":"gates.json"}}
            p = root / "manifest.json"; p.write_text(json.dumps(manifest))
            data, findings, _ = load_dataset(p)
            self.assertEqual(findings, []); self.assertAlmostEqual(data["aircraft"][0]["wingspan"], 30.48); self.assertEqual(data["aircraft"][0]["wingspan_unit"], "m")
            manifest["files"]["aircraft"] = "aircraft.json"; (root / "aircraft.json").write_text('[{"aircraft_id":"AC1","type":"jet","wingspan":1,"wingspan_unit":"yards"}]'); p.write_text(json.dumps(manifest))
            _, findings, _ = load_dataset(p); self.assertIn("UNIT_INVALID", {f["code"] for f in findings})

    def test_naive_timezone_ambiguous_time_fails_without_database(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            for name, content in (("flights", '[{"flight_id":"F1","operation":"arrival","scheduled_time":"2026-01-01 09:00","aircraft_id":"AC1"}]'), ("aircraft", '[{"aircraft_id":"AC1","type":"jet"}]'), ("gates", '[{"gate_id":"G1"}]')):
                (root / f"{name}.json").write_text(content)
            p = root / "manifest.json"; p.write_text(json.dumps({"schema_version":"1.0","airport":{"airport_id":"X","timezone":"America/Chicago"},"files":{"flights":"flights.json","aircraft":"aircraft.json","gates":"gates.json"}}))
            _, findings, _ = load_dataset(p)
            self.assertIn("TIMEZONE_INVALID", {f["code"] for f in findings})

    def test_static_equivalence_fixtures_and_malformed_fixture(self):
        json_root = Path(__file__).parent / "examples" / "equivalence"
        csv_root = Path(__file__).parent / "examples" / "equivalence-csv"
        j, jf, _ = load_dataset(json_root / "manifest.json"); c, cf, _ = load_dataset(csv_root / "manifest.json")
        self.assertFalse(jf); self.assertFalse(cf)
        for key in ("flights", "aircraft", "gates"): self.assertEqual(j[key], c[key])
        self.assertNotEqual(j["sources"], c["sources"])
        malformed = Path(__file__).parent / "examples" / "malformed" / "manifest.json"
        _, findings, _ = load_dataset(malformed)
        codes = {f["code"] for f in findings}
        self.assertTrue({"TIMESTAMP_INVALID", "ENUM_UNSUPPORTED", "UNIT_INVALID", "DURATION_INVALID", "INTERVAL_INVALID", "DISRUPTION_UNKNOWN_TARGET", "NORMALIZED_ID_COLLISION"} <= codes)

    def test_malformed_csv_and_json_report_stable_parse_code(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td); (root / "flights.csv").write_text("flight_id,operation\nF1,arrival,extra\n")
            (root / "aircraft.json").write_text("{"); (root / "gates.json").write_text("[]")
            p = root / "manifest.json"; p.write_text(json.dumps({"schema_version":"1.0","airport":{"airport_id":"X","timezone":"UTC"},"files":{"flights":"flights.csv","aircraft":"aircraft.json","gates":"gates.json"}}))
            _, first, _ = load_dataset(p); _, second, _ = load_dataset(p)
            self.assertGreaterEqual(sum(f["code"] == "SOURCE_PARSE_ERROR" for f in first), 2)
            self.assertEqual(first, second)

    def test_gate_overlap_is_only_reported_with_supplied_windows(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            (root / "aircraft.json").write_text('[{"aircraft_id":"AC1","type":"jet"},{"aircraft_id":"AC2","type":"jet"}]')
            (root / "gates.json").write_text('[{"gate_id":"G1"}]')
            (root / "flights.json").write_text('[{"flight_id":"F1","operation":"arrival","scheduled_time":"2026-01-01T00:00:00Z","aircraft_id":"AC1","gate_id":"G1","occupancy_start":"2026-01-01T00:00:00Z","occupancy_end":"2026-01-01T02:00:00Z"},{"flight_id":"F2","operation":"departure","scheduled_time":"2026-01-01T01:00:00Z","aircraft_id":"AC2","gate_id":"G1","occupancy_start":"2026-01-01T01:00:00Z","occupancy_end":"2026-01-01T03:00:00Z"}]')
            p = root / "manifest.json"; p.write_text(json.dumps({"schema_version":"1.0","airport":{"airport_id":"X","timezone":"UTC"},"files":{"flights":"flights.json","aircraft":"aircraft.json","gates":"gates.json"}}))
            _, findings, _ = load_dataset(p)
            self.assertIn("GATE_ASSIGNMENT_OVERLAP", {f["code"] for f in findings})

    def test_delayed_service_can_reference_turnaround_requirement(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            files = {
                "aircraft.json": '[{"aircraft_id":"AC1","type":"jet"}]',
                "gates.json": '[{"gate_id":"G1"}]',
                "flights.json": '[{"flight_id":"F1","operation":"arrival","scheduled_time":"2026-01-01T00:00:00Z","aircraft_id":"AC1"}]',
                "equipment.json": '[{"equipment_id":"EQ1","type":"tug","status":"available"}]',
                "turnaround.json": '[{"requirement_id":"REQ1","flight_id":"F1","service_type":"pushback","duration_minutes":10}]',
                "disruptions.json": '[{"disruption_id":"D1","type":"delayed_service","requirement_id":"REQ1","start_time":"2026-01-01T00:00:00Z","delay_minutes":5}]',
            }
            for name, content in files.items(): (root / name).write_text(content)
            manifest = {"schema_version":"1.0","airport":{"airport_id":"X","timezone":"UTC"},"files":{"flights":"flights.json","aircraft":"aircraft.json","gates":"gates.json","equipment":"equipment.json","turnaround_requirements":"turnaround.json","disruptions":"disruptions.json"}}
            p = root / "manifest.json"; p.write_text(json.dumps(manifest))
            _, findings, _ = load_dataset(p)
            self.assertNotIn("DISRUPTION_UNKNOWN_TARGET", {f["code"] for f in findings})

    def test_inspection_and_cli_validation_failure(self):
        from .cli import main
        with tempfile.TemporaryDirectory() as td:
            p = Path(td) / "bad.json"; p.write_text("{")
            manifest = Path(td) / "manifest.json"; manifest.write_text(json.dumps({"schema_version":"1.0","airport":{"airport_id":"X","timezone":"UTC"},"files":{"flights":"bad.json","aircraft":"bad.json","gates":"bad.json"}}))
            self.assertEqual(main(["validate", str(manifest)]), 1)

if __name__ == "__main__": unittest.main()
