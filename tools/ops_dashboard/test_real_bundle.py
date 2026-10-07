import csv
import json
import subprocess
import tempfile
import unittest
from pathlib import Path

from run_real_experiment import generate
from validate_real_bundle import validate_bundle

ROOT = Path(__file__).resolve().parents[2]
CLI_CANDIDATES = (ROOT / "build" / "Release" / "airside_cli.exe",
                  ROOT / "build" / "airside_cli.exe",
                  ROOT / "build-final-msvc" / "Release" / "airside_cli.exe")
CLI = next((path for path in CLI_CANDIDATES if path.is_file()), CLI_CANDIDATES[1])


class RealBundleValidationTests(unittest.TestCase):
    def make_bundle(self, root: Path) -> Path:
        root.mkdir()
        task_rows = [{"task_id": index, "service_type": "Fueling", "state": "Completed",
                      "requested_at_seconds": 0, "started_at_seconds": 0, "completed_at_seconds": 30}
                     for index in (1, 2)]
        native = {"seed": 42, "simulated_duration_seconds": 900, "total_turnarounds": 1,
                  "completed_turnarounds": 1, "delayed_turnarounds": 1,
                  "failed_or_timed_out_turnarounds": 0,
                  "fleet": {"collisions": 0, "minimum_separation_m": 5.0},
                  "surface": {"departed_aircraft": 1, "total_aircraft": 1, "taxi_distance_m": 100,
                              "aircraft_aircraft_collisions": 0, "aircraft_ground_collisions": 0,
                              "minimum_aircraft_separation_m": 10.0},
                  "aircraft_operations": [{"aircraft_id": 1, "flight_number": "AX101",
                      "operation_type": "departure", "surface_state": "Departed", "taxi_distance_m": 100}],
                  "turnarounds": [{"aircraft_id": 1, "turnaround_id": "TO-AX101",
                      "estimated_ready_time_seconds": 600, "actual_arrival_seconds": 0,
                      "departure_delay_seconds": 100, "completed_required_tasks": 2, "tasks": task_rows}]}
        row = {"ordinal": 1, "case_id": "case_0001", "seed": 42, "replication": 1,
               "scenario": "mixed_runway_operations", "simulated_duration_seconds": 900,
               "aircraft_count": 1, "surface_total_aircraft": 1,
               "surface_departed_aircraft": 1, "total_turnarounds": 1, "delayed_aircraft": 1,
               "avg_departure_delay_minutes": 100 / 60, "avg_turnaround_minutes": 10,
               "completed_service_tasks": 2, "service_task_count": 2, "disruption_events": 1,
               "road_closure_events": 1, "event_count": 4, "surface_taxi_distance_m": 100,
               "surface_aircraft_aircraft_collisions": 0, "surface_aircraft_ground_collisions": 0,
               "minimum_aircraft_separation_m": 10}
        metadata = {"schema_version": 1, "experiment_name": "goal21_mixed_runway_acceptance",
                    "scenario_name": row["scenario"], "source_scenario": "scenarios/mixed_runway_operations.yaml",
                    "seed": 42, "run_count": 1, "runs": [row]}
        (root / "experiment.json").write_text(json.dumps(metadata), encoding="utf-8")
        with (root / "runs.csv").open("w", newline="", encoding="utf-8") as handle:
            writer = csv.DictWriter(handle, fieldnames=list(row))
            writer.writeheader()
            writer.writerow(row)
        aircraft = {"aircraft_id": 1, "flight_number": "AX101", "operation_type": "departure",
                    "state": "Departed", "surface_state": "Departed", "taxi_distance_m": 100,
                    "departure_delay_seconds": 100, "turnaround_duration_seconds": 600,
                    "runway_wait_seconds": 0, "departure_time_seconds": 900,
                    "task_count": 2, "completed_tasks": 2, "service_waiting_seconds": 0}
        with (root / "aircraft.csv").open("w", newline="", encoding="utf-8") as handle:
            writer = csv.DictWriter(handle, fieldnames=list(aircraft))
            writer.writeheader()
            writer.writerow(aircraft)
        events = [{"sequence": 0, "time_seconds": 30, "type": "TurnaroundTaskCompleted"},
                  {"sequence": 1, "time_seconds": 60, "type": "TurnaroundTaskCompleted"},
                  {"sequence": 2, "time_seconds": 300, "type": "RoadClosed"},
                  {"sequence": 3, "time_seconds": 900, "type": "AircraftDeparted"}]
        (root / "events.jsonl").write_text("\n".join(json.dumps(event) for event in events) + "\n", encoding="utf-8")
        (root / "simulator-metrics.json").write_text(json.dumps(native), encoding="utf-8")
        native_metrics = {"seed": 42, "simulated_duration_seconds": 900,
                          "surface_departed_aircraft": 1, "surface_total_aircraft": 1,
                          "arrival_taxi_distance_m": 100, "departure_taxi_distance_m": 0,
                          "surface_aircraft_aircraft_collisions": 0,
                          "surface_aircraft_ground_collisions": 0,
                          "minimum_aircraft_separation_m": 10}
        with (root / "simulator-metrics.csv").open("w", newline="", encoding="utf-8") as handle:
            writer = csv.DictWriter(handle, fieldnames=list(native_metrics))
            writer.writeheader()
            writer.writerow(native_metrics)
        native_aircraft = {key: aircraft[key] for key in ("aircraft_id", "flight_number", "operation_type",
                           "surface_state", "taxi_distance_m", "runway_wait_seconds", "departure_time_seconds")}
        with (root / "simulator-metrics.aircraft.csv").open("w", newline="", encoding="utf-8") as handle:
            writer = csv.DictWriter(handle, fieldnames=list(native_aircraft))
            writer.writeheader()
            writer.writerow(native_aircraft)
        return root

    def test_consistent_bundle_validates_goal18_safety_and_surface_exports(self):
        with tempfile.TemporaryDirectory() as temporary:
            result = validate_bundle(self.make_bundle(Path(temporary) / "run"))
        self.assertEqual(result["aircraft"], 1)
        self.assertEqual(result["departed"], 1)
        self.assertEqual(result["completed_service_tasks"], 2)
        self.assertEqual(result["road_closure_events"], 1)
        self.assertEqual(result["aircraft_collisions"], 0)
        self.assertEqual(result["minimum_aircraft_separation_m"], 10.0)

    def test_missing_bundle_artifact_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = self.make_bundle(Path(temporary) / "run")
            (root / "events.jsonl").unlink()
            with self.assertRaisesRegex(ValueError, "events.jsonl"):
                validate_bundle(root)

    def test_event_mismatch_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = self.make_bundle(Path(temporary) / "run")
            (root / "events.jsonl").write_text(
                '{"sequence":0,"time_seconds":900,"type":"AircraftDeparted"}\n', encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "TurnaroundTaskCompleted"):
                validate_bundle(root)

    def test_cross_artifact_taxi_mismatch_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = self.make_bundle(Path(temporary) / "run")
            with (root / "aircraft.csv").open(newline="", encoding="utf-8") as handle:
                rows = list(csv.DictReader(handle))
            rows[0]["taxi_distance_m"] = "99"
            with (root / "aircraft.csv").open("w", newline="", encoding="utf-8") as handle:
                writer = csv.DictWriter(handle, fieldnames=list(rows[0]))
                writer.writeheader()
                writer.writerows(rows)
            with self.assertRaisesRegex(ValueError, "taxi distance"):
                validate_bundle(root)

    @unittest.skipUnless(CLI.is_file(), "build airside_cli to run simulator export integration")
    def test_real_control_disruption_and_repeats_are_packaged_and_safe(self):
        with tempfile.TemporaryDirectory() as temporary:
            result = generate(CLI, Path(temporary), 42)
        comparison = result["comparison"]
        self.assertEqual(comparison["safety"]["status"], "no_regression")
        self.assertEqual(result["bundle_validation"]["control"]["aircraft_collisions"], 0)
        self.assertEqual(result["bundle_validation"]["disruption"]["aircraft_collisions"], 0)
        self.assertNotEqual(result["determinism"]["control"]["status"], "different")
        changes = {metric["key"]: metric for metric in comparison["metrics"]}
        self.assertGreater(changes["surface_taxi_distance_m"]["difference"], 0)
        self.assertEqual(changes["road_closure_events"]["difference"], 1)


if __name__ == "__main__":
    unittest.main()
