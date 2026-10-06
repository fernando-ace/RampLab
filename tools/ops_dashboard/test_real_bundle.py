import csv
import json
import subprocess
import tempfile
import unittest
from pathlib import Path

from validate_real_bundle import validate_bundle

ROOT = Path(__file__).resolve().parents[2]
CLI = ROOT / "build" / "airside_cli.exe"


class RealBundleValidationTests(unittest.TestCase):
    def make_bundle(self, root: Path) -> Path:
        root.mkdir()
        run = {"ordinal": 1, "case_id": "case_0001", "seed": 42, "replication": 1,
               "scenario": "baseline", "simulated_duration_seconds": 900,
               "avg_turnaround_minutes": 15, "avg_departure_delay_minutes": 100 / 60,
               "avg_service_waiting_minutes": 0, "delayed_aircraft": 0, "aircraft_count": 1,
               "fuel_utilization": 0.5, "baggage_utilization": 0.5,
               "service_task_count": 2, "completed_service_tasks": 2, "disruption_events": 1,
               "road_closure_events": 1, "event_count": 4}
        (root / "experiment.json").write_text(json.dumps({"schema_version": 1, "experiment_name": "baseline",
            "scenario_name": "baseline", "source_scenario": "scenarios/baseline.yaml", "seed": 42,
            "run_count": 1, "runs": [run]}), encoding="utf-8")
        aircraft = {"aircraft_id": 1, "flight_number": "AX101", "state": "Departed",
                    "scheduled_arrival_seconds": 0, "scheduled_departure_seconds": 800,
                    "actual_arrival_seconds": 0, "actual_departure_seconds": 900,
                    "turnaround_duration_seconds": 900, "departure_delay_seconds": 100,
                    "service_waiting_seconds": 0, "task_count": 2, "completed_tasks": 2}
        # Adjust run-level delayed aircraft to match this aircraft row.
        run["delayed_aircraft"] = 1
        metadata = json.loads((root / "experiment.json").read_text(encoding="utf-8"))
        metadata["runs"][0]["delayed_aircraft"] = 1
        (root / "experiment.json").write_text(json.dumps(metadata), encoding="utf-8")
        with (root / "runs.csv").open("w", newline="", encoding="utf-8") as handle:
            writer = csv.DictWriter(handle, fieldnames=run.keys())
            writer.writeheader()
            writer.writerow(run)
        with (root / "aircraft.csv").open("w", newline="", encoding="utf-8") as handle:
            writer = csv.DictWriter(handle, fieldnames=aircraft.keys())
            writer.writeheader()
            writer.writerow(aircraft)
        events = [{"sequence": 1, "time_seconds": 10, "type": "ServiceCompleted"},
                  {"sequence": 2, "time_seconds": 20, "type": "ServiceCompleted"},
                  {"sequence": 3, "time_seconds": 300, "type": "RoadClosed"},
                  {"sequence": 4, "time_seconds": 900, "type": "AircraftDeparted"}]
        (root / "events.jsonl").write_text("\n".join(json.dumps(event) for event in events) + "\n", encoding="utf-8")
        return root

    def test_consistent_bundle_validates_and_reports_unknown_collision_data(self):
        with tempfile.TemporaryDirectory() as temporary:
            result = validate_bundle(self.make_bundle(Path(temporary) / "run"))
        self.assertEqual(result["aircraft"], 1)
        self.assertEqual(result["departed"], 1)
        self.assertEqual(result["completed_service_tasks"], 2)
        self.assertEqual(result["road_closure_events"], 1)
        self.assertEqual(result["collision_data"], "unknown (not exported by this simulator)")

    def test_missing_bundle_artifact_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = self.make_bundle(Path(temporary) / "run")
            (root / "events.jsonl").unlink()
            with self.assertRaisesRegex(ValueError, "events.jsonl"):
                validate_bundle(root)

    def test_event_count_mismatch_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = self.make_bundle(Path(temporary) / "run")
            (root / "events.jsonl").write_text('{"sequence":1,"time_seconds":5,"type":"AircraftDeparted"}\n',
                                                encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "ServiceCompleted"):
                validate_bundle(root)

    def test_out_of_order_event_time_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = self.make_bundle(Path(temporary) / "run")
            (root / "events.jsonl").write_text(
                '{"sequence":1,"time_seconds":20,"type":"ServiceCompleted"}\n'
                '{"sequence":2,"time_seconds":10,"type":"ServiceCompleted"}\n'
                '{"sequence":3,"time_seconds":300,"type":"RoadClosed"}\n'
                '{"sequence":4,"time_seconds":900,"type":"AircraftDeparted"}\n', encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "timestamps"):
                validate_bundle(root)

    @unittest.skipUnless(CLI.is_file(), "build airside_cli to run simulator export integration")
    def test_real_simulator_control_and_closure_exports(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            bundles = {}
            for name, disabled in (("control", True), ("disruption", False)):
                destination = root / name
                command = [str(CLI), "--scenario", "scenarios/baseline.yaml", "--seed", "42",
                           "--quiet", "--export-run-dir", str(destination)]
                if disabled:
                    command.append("--disable-road-events")
                subprocess.run(command, cwd=ROOT, check=True, capture_output=True, text=True)
                bundles[name] = validate_bundle(destination)
            self.assertEqual(bundles["control"]["disruption_events"], 0)
            self.assertEqual(bundles["disruption"]["disruption_events"], 1)


if __name__ == "__main__":
    unittest.main()
