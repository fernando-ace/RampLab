"""Isolated operator-dashboard data and local API tests."""

from __future__ import annotations

import json
import sys
import threading
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory
from urllib.request import Request, urlopen
from http.server import ThreadingHTTPServer

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT))
import server


def fixture_run(name: str) -> dict:
    folder = ROOT / "fixtures" / name
    return {"id": name, "label": name.title(), "files": [
        {"name": p.name, "content": p.read_text(encoding="utf-8")}
        for p in sorted(folder.iterdir()) if p.is_file()
    ]}


def analyze(*runs, determinism=False):
    with TemporaryDirectory() as temp:
        loaded = [server._run_data(run, Path(temp) / f"run-{i}") for i, run in enumerate(runs)]
        result = {"runs": [item["report"] for item in loaded], "analysis": None, "markdown": None, "determinism": None, "event_determinism": None}
        if len(loaded) == 2 and all(item["primary"] for item in loaded):
            left, right = loaded[0]["primary"], loaded[1]["primary"]
            result["analysis"] = server.analyzer.compare(left, right)
            result["markdown"] = server.analyzer.markdown(result["analysis"], left, right)
            if determinism:
                result["determinism"] = server.analyzer.determinism(left, right)
                if loaded[0]["event_run"] and loaded[1]["event_run"]:
                    result["event_determinism"] = server.analyzer.determinism(loaded[0]["event_run"], loaded[1]["event_run"])
        return result


class DashboardDataTests(unittest.TestCase):
    def test_real_demo_label_uses_generated_airport_and_release_run(self):
        with TemporaryDirectory() as temp:
            bundle = Path(temp)
            (bundle / "release-run.json").write_text(json.dumps({"mode": "disruption", "seed": 42,
                                                                    "scenario": "airport_kauo"}), encoding="utf-8")
            (bundle / "experiment.json").write_text(json.dumps({
                "source_scenario": "airport_kauo",
                "scenario_generation": {"manifest": {"airport": {"airport_id": "KAUO", "icao": "KAUO"}}},
            }), encoding="utf-8")
            self.assertEqual(server._real_demo_label(bundle, "fallback"),
                             "Disruption · KAUO · airport_kauo · seed 42")

    def test_valid_control_loads_metrics_entities_and_events(self):
        report = analyze(fixture_run("control"))["runs"][0]
        self.assertEqual(report["metrics"]["surface_departed_aircraft"], 3)
        self.assertEqual(len(report["aircraft"]), 3)
        self.assertEqual(len(report["events"]), 5)
        self.assertTrue(report["event_history_available"])
        self.assertEqual(report["safety"]["status"], "no_collisions_observed")
        self.assertEqual(report["files"][0]["status"], "loaded")

    def test_malformed_file_is_reported_not_silently_used(self):
        run = {"id": "bad", "label": "Bad input", "files": [{"name": "summary.json", "content": "{"}]}
        report = analyze(run)["runs"][0]
        self.assertEqual(report["files"][0]["status"], "error")
        self.assertFalse(report["metrics"])
        self.assertEqual(report["safety"]["status"], "unknown")

    def test_missing_optional_fields_and_missing_collision_fields_are_unknown(self):
        run = {"id": "partial", "label": "Partial", "files": [{"name": "summary.json", "content": '{"scenario_name":"partial","seed":5,"surface_wait_seconds":0}'}]}
        report = analyze(run)["runs"][0]
        self.assertEqual(report["safety"]["status"], "unknown")
        self.assertIn("CSV", report["missing_optional"])
        self.assertIn("JSONL", report["missing_optional"])
        self.assertIn("absent", report["safety"]["findings"][0])

    def test_zero_history_is_intentionally_unavailable(self):
        run = {"id": "no-events", "label": "No events", "files": [{"name": "summary.json", "content": '{"surface_reroutes":0}'}]}
        report = analyze(run)["runs"][0]
        self.assertFalse(report["event_history_available"])
        self.assertEqual(report["events"], [])

    def test_jsonl_events_are_sorted_chronologically(self):
        run = {"id": "ordered", "label": "Ordered", "files": [
            {"name": "summary.json", "content": '{"surface_reroutes":0}'},
            {"name": "events.jsonl", "content": '{"time_seconds":9,"type":"late"}\n{"time_seconds":2,"type":"early"}\n'}
        ]}
        events = analyze(run)["runs"][0]["events"]
        self.assertEqual([e["time_seconds"] for e in events], [2, 9])

    def test_control_vs_disruption_uses_goal19_comparison_and_ranking(self):
        result = analyze(fixture_run("control"), fixture_run("disruption"))
        analysis = result["analysis"]
        self.assertEqual(analysis["shared_metric_count"], 54)
        distance = next(x for x in analysis["metrics"] if x["key"] == "surface_taxi_distance_m")
        self.assertEqual(distance["difference"], 263)
        self.assertEqual(distance["impact"], "regression")
        self.assertEqual(analysis["operational_impact"][0]["key"], "departure_runway_wait_seconds")
        self.assertIn("## Safety", result["markdown"])
        self.assertEqual(analysis["safety"]["status"], "regression")

    def test_zero_baseline_percentage_stays_null(self):
        result = analyze(fixture_run("control"), fixture_run("disruption"))["analysis"]
        reroute = next(x for x in result["metrics"] if x["key"] == "surface_reroutes")
        self.assertEqual(reroute["difference"], 1)
        self.assertIsNone(reroute["percent_difference"])

    def test_determinism_is_returned_when_requested(self):
        result = analyze(fixture_run("control"), fixture_run("control"), determinism=True)
        self.assertEqual(result["determinism"]["status"], "byte_identical")
        self.assertEqual(result["event_determinism"]["status"], "byte_identical")

    def test_event_determinism_reports_ordered_record_differences(self):
        result = analyze(fixture_run("control"), fixture_run("disruption"), determinism=True)
        self.assertEqual(result["event_determinism"]["status"], "different")
        self.assertTrue(result["event_determinism"]["changed_records"])

    def test_unsupported_file_is_reported(self):
        run = {"id": "unsupported", "label": "Unsupported", "files": [{"name": "notes.txt", "content": "hello"}]}
        report = analyze(run)["runs"][0]
        self.assertEqual(report["files"][0]["status"], "unsupported")


class DashboardHttpTests(unittest.TestCase):
    def test_local_analysis_endpoint_runs_demo_comparison(self):
        server_instance = ThreadingHTTPServer(("127.0.0.1", 0), server.Handler)
        thread = threading.Thread(target=server_instance.serve_forever, daemon=True)
        thread.start()
        try:
            body = json.dumps({"runs": [fixture_run("control"), fixture_run("disruption")], "determinism": True}).encode()
            req = Request(f"http://127.0.0.1:{server_instance.server_port}/api/analyze", data=body,
                          headers={"Content-Type": "application/json"}, method="POST")
            with urlopen(req, timeout=5) as response:
                payload = json.loads(response.read())
            self.assertEqual(len(payload["runs"]), 2)
            self.assertGreater(payload["analysis"]["shared_metric_count"], 10)
            self.assertIn("ranked change(s)", payload["markdown"])
            self.assertIn("## Event-stream determinism", payload["markdown"])
            self.assertEqual(payload["event_determinism"]["status"], "different")
        finally:
            server_instance.shutdown()
            server_instance.server_close()
            thread.join(timeout=2)


if __name__ == "__main__":
    unittest.main()
