import json
import tempfile
import unittest
from pathlib import Path

from analyze import compare, determinism, load_run, main, markdown


def write_json(path: Path, metrics: dict, *, seed=7, name="case") -> Path:
    path.write_text(json.dumps({"schema_version": 1, "experiment_name": name, "run_count": 1,
                               "runs": [{"ordinal": 1, "case_id": "case_0001", "seed": seed, **metrics}]}), encoding="utf-8")
    return path


class AnalysisTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def test_single_run_parsing_and_optional_fields(self):
        path = write_json(self.root / "run.json", {"mean_departure_delay_seconds": 10})
        run = load_run(path)
        self.assertEqual(run.metrics["mean_departure_delay_seconds"], 10)
        self.assertNotIn("fleet_collisions", run.metrics)

    def test_two_run_comparison_and_zero_baseline_percentage(self):
        left = load_run(write_json(self.root / "left.json", {"surface_reroutes": 0, "surface_wait_seconds": 10}))
        right = load_run(write_json(self.root / "right.json", {"surface_reroutes": 2, "surface_wait_seconds": 5}))
        report = compare(left, right)
        reroutes = next(x for x in report["metrics"] if x["key"] == "surface_reroutes")
        self.assertIsNone(reroutes["percent_difference"])
        wait = next(x for x in report["metrics"] if x["key"] == "surface_wait_seconds")
        self.assertEqual(wait["percent_difference"], -50)
        self.assertTrue(report["compatibility"]["comparable"])

    def test_road_closure_event_metric_is_compared_as_a_disruption(self):
        left = load_run(write_json(self.root / "control.json", {"road_closure_events": 0}))
        right = load_run(write_json(self.root / "disruption.json", {"road_closure_events": 1}))
        closure = next(metric for metric in compare(left, right)["metrics"]
                       if metric["key"] == "road_closure_events")
        self.assertEqual(closure["category"], "disruptions")
        self.assertEqual(closure["difference"], 1)
        self.assertEqual(closure["impact"], "regression")

    def test_safety_regression_and_missing_collision_unknown(self):
        left = load_run(write_json(self.root / "left.json", {"fleet_collisions": 0, "minimum_aircraft_separation_m": 4,
                                                                    "failed_or_timed_out_turnarounds": 0}))
        right = load_run(write_json(self.root / "right.json", {"fleet_collisions": 1, "minimum_aircraft_separation_m": 3,
                                                                     "failed_or_timed_out_turnarounds": 1}))
        self.assertEqual(compare(left, right)["safety"]["status"], "regression")
        unknown_a = load_run(write_json(self.root / "unknown-a.json", {"surface_wait_seconds": 0}))
        unknown_b = load_run(write_json(self.root / "unknown-b.json", {"surface_wait_seconds": 0}))
        self.assertEqual(compare(unknown_a, unknown_b)["safety"]["status"], "unknown")

    def test_determinism_structured_and_byte_identical(self):
        one = write_json(self.root / "one.json", {"surface_wait_seconds": 2})
        two = self.root / "two.json"
        two.write_bytes(one.read_bytes())
        self.assertEqual(determinism(load_run(one), load_run(two))["status"], "byte_identical")
        two.write_text(two.read_text().replace('"surface_wait_seconds": 2', '"surface_wait_seconds": 3'), encoding="utf-8")
        result = determinism(load_run(one), load_run(two))
        self.assertEqual(result["status"], "different")
        self.assertEqual(result["changed_fields"][0]["field"], "surface_wait_seconds")

    def test_determinism_ignores_observational_timing_and_checks_sibling_csv(self):
        run_a, run_b = self.root / "a", self.root / "b"
        run_a.mkdir()
        run_b.mkdir()
        one = write_json(run_a / "experiment.json", {"surface_wait_seconds": 2})
        two = write_json(run_b / "experiment.json", {"surface_wait_seconds": 2})
        header = "run_ordinal,execution_ms,surface_wait_seconds\n"
        (run_a / "runs.csv").write_text(header + "1,2.5,2\n", encoding="utf-8")
        (run_b / "runs.csv").write_text(header + "1,3.5,2\n", encoding="utf-8")
        left = load_run(one)
        first = determinism(left, load_run(two))
        self.assertEqual(first["status"], "equivalent")
        (run_b / "runs.csv").write_text(header + "1,4.5,3\n", encoding="utf-8")
        second = determinism(left, load_run(two))
        self.assertNotEqual(second["status"], "byte_identical")
        self.assertEqual(second["status"], "different")
        self.assertEqual(second["changed_fields"][0]["field"], "surface_wait_seconds")

    def test_mismatched_runs_report_identity(self):
        left = load_run(write_json(self.root / "left.json", {"surface_wait_seconds": 1}, seed=1))
        right = load_run(write_json(self.root / "right.json", {"surface_wait_seconds": 2}, seed=2))
        self.assertEqual(compare(left, right)["compatibility"]["mismatched_identity"], ["seed"])

    def test_markdown_and_json_report_generation(self):
        one = write_json(self.root / "one.json", {"surface_wait_seconds": 2})
        two = write_json(self.root / "two.json", {"surface_wait_seconds": 3})
        md, js = self.root / "comparison.md", self.root / "comparison.json"
        self.assertEqual(main([str(one), str(two), "--output", str(md), "--json", str(js)]), 0)
        self.assertIn("## Safety", md.read_text(encoding="utf-8"))
        self.assertEqual(json.loads(js.read_text(encoding="utf-8"))["shared_metric_count"], 1)

    def test_jsonl_event_counts(self):
        path = self.root / "events.jsonl"
        path.write_text('{"type":"reroute"}\n{"type":"reroute"}\n{"type":"arrival"}\n', encoding="utf-8")
        run = load_run(path)
        self.assertEqual(run.raw["event_counts"], {"reroute": 2, "arrival": 1})

    def test_summary_csv_metrics(self):
        path = self.root / "summary.csv"
        path.write_text("case_id,turnaround_minutes_count,turnaround_minutes_mean,probability_any_delay\ncase_0001,3,12.5,0.25\n", encoding="utf-8")
        run = load_run(path)
        self.assertEqual(run.metrics["turnaround_minutes_mean"], 12.5)
        self.assertEqual(run.metrics["probability_any_delay"], 0.25)


if __name__ == "__main__":
    unittest.main()
