from __future__ import annotations

import csv
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from tools.airport_scenario_generation.run_artifacts import write_determinism_report, write_goal19_artifacts
from tools.evidence_bundle.bundle import build as build_evidence_bundle
from tools.evidence_bundle.bundle import inspect as inspect_evidence_run
from tools.experiment_analysis.analyze import determinism, load_run
from tools.release.ramplab import ROOT, run_scenario, scenario_path_for_unreal


class RunArtifactAdapterTests(unittest.TestCase):
    def test_unreal_path_uses_selected_scenario_or_generated_override(self):
        catalog_path = ROOT / "scenarios" / "mixed_runway_operations.yaml"
        self.assertEqual(scenario_path_for_unreal("mixed_runway_operations"), catalog_path.resolve())

        with tempfile.TemporaryDirectory() as temporary:
            generated = Path(temporary) / "scenario.json"
            generated.write_text('{"name":"airport_syn1"}', encoding="utf-8")
            self.assertEqual(scenario_path_for_unreal("ignored", generated), generated.resolve())

    def test_release_runner_keeps_native_outputs_and_adds_goal19_views(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            scenario_file = root / "generated" / "scenario.json"
            scenario_file.parent.mkdir()
            scenario_file.write_text(json.dumps({"name": "airport_syn1"}), encoding="utf-8")
            output = root / "run"

            def simulate(args, cwd=None):
                json_path = Path(args[args.index("--metrics-json") + 1])
                csv_path = Path(args[args.index("--metrics-csv") + 1])
                events_path = Path(args[args.index("--record-events") + 1])
                json_path.write_text(json.dumps({
                    "seed": 42,
                    "simulated_duration_seconds": 60,
                    "total_turnarounds": 1,
                    "completed_turnarounds": 1,
                    "fleet": {"collisions": 0, "minimum_separation_m": 5.0},
                    "surface": {"total_aircraft": 1, "departed_aircraft": 1},
                    "aircraft_operations": [{"aircraft_id": 1, "flight_number": "F1"}],
                }), encoding="utf-8")
                csv_path.write_text("seed,simulated_duration_seconds\n42,60\n", encoding="utf-8")
                events_path.write_text('{"type":"AircraftDeparted","time_seconds":60}\n', encoding="utf-8")

            with patch("tools.release.ramplab.command", side_effect=simulate):
                metadata = run_scenario(Path("unused-airside-cli"), None, "disruption", 42, output, scenario_file)

            self.assertEqual(metadata["scenario"], "airport_syn1")
            for filename in (
                "simulator-metrics.json",
                "simulator-metrics.csv",
                "events.jsonl",
                "experiment.json",
                "runs.csv",
                "aircraft.csv",
                "release-run.json",
            ):
                self.assertTrue((output / filename).is_file(), filename)
            self.assertIn("experiment.json", metadata["artifacts"])

    def test_native_outputs_feed_goal19_goal20_and_goal22_with_provenance(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            scenario_dir = root / "generated"
            output = root / "run"
            scenario_dir.mkdir()
            output.mkdir()
            scenario_file = scenario_dir / "scenario.json"
            scenario_file.write_text(json.dumps({"name": "airport_syn1"}), encoding="utf-8")
            manifest = {
                "source_canonical_sha256": "canonical-hash",
                "mapping_configuration_sha256": "mapping-hash",
                "geometry_configuration_sha256": "geometry-hash",
                "generated_scenario_id": "scenario-id",
                "output_file_sha256": {"scenario.json": "scenario-hash"},
                "seed": 42,
            }
            identity_map = {"flights": {"SRC-F1": "SIM-F1"}}
            support_matrix = {"gates": {"status": "supported with transformation"}}
            for name, value in (
                ("manifest.json", manifest),
                ("identity-map.json", identity_map),
                ("support-matrix.json", support_matrix),
            ):
                (scenario_dir / name).write_text(json.dumps(value), encoding="utf-8")

            native = {
                "seed": 42,
                "simulated_duration_seconds": 3600,
                "total_turnarounds": 1,
                "completed_turnarounds": 1,
                "delayed_turnarounds": 0,
                "failed_or_timed_out_turnarounds": 0,
                "task_reassignments": 0,
                "disruption_triggered_replans": 0,
                "unresolved_service_requests": 0,
                "fleet": {
                    "collisions": 0,
                    "minimum_separation_m": 4.25,
                    "reservation_contentions": 0,
                    "outstanding_reservations": 0,
                    "unfinished_requests": 0,
                    "reassignments": 0,
                    "requests_created": 2,
                    "requests_completed": 2,
                    "requests_failed": 0,
                },
                "surface": {
                    "departed_aircraft": 1,
                    "total_aircraft": 1,
                    "reroutes": 0,
                    "wait_events": 0,
                    "wait_seconds": 0,
                    "taxi_distance_m": 125.0,
                    "taxi_seconds": 120,
                    "runway_queue_seconds": 0,
                    "safe_failures": 0,
                    "max_simultaneous_taxiing": 1,
                    "minimum_aircraft_separation_m": 12.0,
                    "minimum_aircraft_ground_separation_m": 6.0,
                    "aircraft_aircraft_collisions": 0,
                    "aircraft_ground_collisions": 0,
                    "arrivals_completed": 1,
                    "runway_operations_completed": 2,
                    "maximum_runway_queue_depth": 1,
                    "arrival_runway_wait_seconds": 0,
                    "departure_runway_wait_seconds": 0,
                    "average_runway_wait_seconds": 0,
                    "runway_utilization": 0.1,
                    "arrival_taxi_seconds": 60,
                    "arrival_taxi_distance_m": 60.0,
                    "departure_taxi_seconds": 60,
                    "departure_taxi_distance_m": 65.0,
                },
                "aircraft_operations": [
                    {
                        "aircraft_id": 1,
                        "flight_number": "F1",
                        "operation_type": "arrival_turnaround",
                        "surface_state": "Departed",
                        "taxi_distance_m": 125.0,
                        "runway_wait_seconds": 5,
                        "arrival_gate_time_seconds": 90,
                        "arrival_taxi_started_at_seconds": 0,
                        "arrival_taxi_completed_at_seconds": 60,
                        "departure_taxi_started_at_seconds": 1000,
                        "departure_taxi_completed_at_seconds": 1060,
                        "departure_time_seconds": 3600,
                    }
                ],
                "turnarounds": [
                    {
                        "aircraft_id": 1,
                        "turnaround_id": "TURN-1",
                        "actual_arrival_seconds": 0,
                        "estimated_ready_time_seconds": 3600,
                        "departure_delay_seconds": 60,
                        "tasks": [
                            {
                                "state": "Completed",
                                "requested_at_seconds": 100,
                                "started_at_seconds": 150,
                            }
                        ],
                    }
                ],
            }
            metrics_path = output / "simulator-metrics.json"
            metrics_path.write_text(json.dumps(native), encoding="utf-8")
            (output / "events.jsonl").write_text(
                '{"type":"AircraftArrived","time_seconds":0}\n', encoding="utf-8"
            )

            names = write_goal19_artifacts(output, metrics_path, "airport_syn1", scenario_file)

            self.assertEqual(names, ["experiment.json", "runs.csv", "aircraft.csv"])
            experiment = json.loads((output / "experiment.json").read_text(encoding="utf-8"))
            self.assertEqual(experiment["source_scenario"], "airport_syn1")
            self.assertEqual(experiment["scenario_generation"]["manifest"], manifest)
            self.assertEqual(experiment["scenario_generation"]["identity_map"], identity_map)
            self.assertEqual(experiment["scenario_generation"]["support_matrix"], support_matrix)
            self.assertEqual(experiment["scenario_generation"]["scenario_document"]["name"], "airport_syn1")
            self.assertEqual(experiment["native_metrics"], native)
            self.assertEqual(experiment["runs"][0]["surface_aircraft_aircraft_collisions"], 0)
            self.assertEqual(experiment["runs"][0]["fleet_minimum_separation_m"], 4.25)
            with (output / "runs.csv").open(newline="", encoding="utf-8") as handle:
                self.assertEqual(next(csv.DictReader(handle))["scenario"], "airport_syn1")

            analyzed = load_run(output / "experiment.json")
            self.assertEqual(analyzed.metrics["surface_total_aircraft"], 1)
            self.assertEqual(analyzed.metrics["fleet_collisions"], 0)
            self.assertEqual(analyzed.metrics["avg_departure_delay_minutes"], 1)
            self.assertEqual(analyzed.metrics["avg_turnaround_minutes"], 60)
            self.assertAlmostEqual(analyzed.metrics["avg_service_waiting_minutes"], 50 / 60)
            self.assertEqual(analyzed.events, [{"type": "AircraftArrived", "time_seconds": 0}])
            with (output / "aircraft.csv").open(newline="", encoding="utf-8") as handle:
                aircraft_row = next(csv.DictReader(handle))
            self.assertEqual(aircraft_row["turnaround_id"], "TURN-1")
            self.assertEqual(aircraft_row["taxi_time_seconds"], "120")
            self.assertEqual(aircraft_row["completed_tasks"], "1")
            repeat_output = root / "repeat"
            repeat_output.mkdir()
            repeat_metrics = repeat_output / "simulator-metrics.json"
            repeat_metrics.write_bytes(metrics_path.read_bytes())
            (repeat_output / "events.jsonl").write_bytes((output / "events.jsonl").read_bytes())
            write_goal19_artifacts(repeat_output, repeat_metrics, "airport_syn1", scenario_file)
            repeated = load_run(repeat_output / "experiment.json")
            self.assertEqual(determinism(analyzed, repeated)["status"], "byte_identical")
            determinism_path = root / "determinism.json"
            report = write_determinism_report(
                output, repeat_output, output, repeat_output, determinism_path
            )
            self.assertEqual(
                {item["status"] for item in report["determinism"].values()},
                {"byte_identical"},
            )
            evidence = inspect_evidence_run("control", output)
            statuses = {item["check"]: item["status"] for item in evidence["findings"]}
            for check in (
                "run_count_consistency",
                "seed_consistency",
                "aircraft_totals",
                "collision_evidence",
                "minimum_separation_evidence",
                "event_file_parseability",
                "scenario_identity",
            ):
                self.assertEqual(statuses[check], "PASS", evidence["findings"])

            bundle = build_evidence_bundle(
                [("synthetic-run", output)], root / "bundle", determinism_path
            )
            bundled_experiment = bundle["output"] / "evidence" / "synthetic-run" / "experiment.json"
            self.assertEqual(bundle["validation"]["hash_verification"]["status"], "PASS")
            self.assertEqual(bundle["manifest"]["determinism_status"], "PASS")
            self.assertEqual(
                bundled_experiment.read_bytes(),
                (output / "experiment.json").read_bytes(),
            )


if __name__ == "__main__":
    unittest.main()
