"""Focused tests for Scenario Studio's canonical-data and run contracts."""
from __future__ import annotations

from copy import deepcopy
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.airport_scenario_generation.generator import generate, validate_generated
from tools.ops_dashboard import scenario_studio as studio


class _QueuedExecutor:
    def __init__(self):
        self.calls = []

    def submit(self, function, *args, **kwargs):
        self.calls.append((function, args, kwargs))


class ScenarioStudioTests(unittest.TestCase):
    def test_scenario_loading_contains_source_data_and_editable_geometry(self):
        payload = studio.bootstrap("kauo")
        self.assertEqual(payload["airport"]["id"], "KAUO")
        self.assertTrue(payload["geometry"]["edges"])
        self.assertEqual(payload["scenario"]["dataset_id"], payload["airport"]["dataset_id"])
        self.assertEqual(payload["scenario"]["disruptions"], [])
        self.assertEqual(set(payload["catalog"]["scenarios"]), set(studio.catalog()["scenarios"]))
        self.assertEqual(payload["runs"], studio.RunManagerSingleton.list())
        self.assertTrue(studio.validate_scenario(payload["scenario"])["valid"])

    def test_variant_diff_does_not_modify_baseline(self):
        baseline = studio.new_scenario("kauo")
        variant = deepcopy(baseline)
        variant["baseline"] = False
        variant["seed"] = 55
        variant["disruptions"].append({"disruption_id": "CLOSE-A", "type": "route_closure",
                                       "resource_id": "KAUO-TWY-A-RAMP-CONNECTOR",
                                       "start_time": "2026-10-09T14:00:15Z", "end_time": "2026-10-09T14:45:00Z"})
        changes = studio.scenario_diff(baseline, variant)
        self.assertEqual(baseline["disruptions"], [])
        self.assertEqual(baseline["seed"], 42)
        self.assertEqual(len(changes), 2)

    def test_flight_edits_change_generated_canonical_inputs(self):
        scenario = studio.new_scenario("kauo")
        scenario["flights"][1]["scheduled_time"] = "2026-10-09T15:30:00Z"
        scenario["flights"][1]["gate_id"] = scenario["gates"][0]["gate_id"]
        package = studio.scenario_package(scenario)
        self.assertEqual(package["flights"][1]["scheduled_time"], "2026-10-09T15:30:00Z")
        self.assertEqual(package["flights"][1]["gate_id"], "GA-PARK-ILLUSTRATIVE")
        result = studio.validate_scenario(scenario)
        self.assertTrue(result["valid"], result["findings"])

    def test_invalid_relationship_and_time_are_blocking_findings(self):
        scenario = studio.new_scenario("kauo")
        scenario["flights"][0]["aircraft_id"] = "UNKNOWN-AIRCRAFT"
        scenario["flights"][0]["scheduled_time"] = "tomorrow morning"
        findings = studio.validate_scenario(scenario)["findings"]
        codes = {item["code"] for item in findings}
        self.assertIn("AIRCRAFT_UNKNOWN", codes)
        self.assertIn("TIME_INVALID", codes)

    def test_closure_target_must_exist_in_generator_mapping(self):
        scenario = studio.new_scenario("kauo")
        scenario["disruptions"] = [{"disruption_id": "CLOSE-B", "type": "route_closure",
                                     "resource_id": "UNMAPPED-TAXIWAY", "start_time": "2026-10-09T14:00:15Z",
                                     "end_time": "2026-10-09T14:45:00Z"}]
        result = studio.validate_scenario(scenario)
        self.assertFalse(result["valid"])
        self.assertIn("ROUTE_UNSUPPORTED", {item["code"] for item in result["findings"]})

    def test_vehicle_outage_needs_a_mapped_vehicle_and_supported_duration(self):
        scenario = studio.new_scenario("kauo")
        scenario["disruptions"] = [{"disruption_id": "OUTAGE", "type": "equipment_outage",
                                     "equipment_id": "NO-VEHICLE", "start_time": "2026-10-09T14:00:15Z"}]
        result = studio.validate_scenario(scenario)
        self.assertFalse(result["valid"])
        self.assertIn("VEHICLE_UNKNOWN", {item["code"] for item in result["findings"]})

    def test_turnaround_duration_and_fleet_fields_are_generator_backed(self):
        scenario = studio.new_scenario("kauo-goal27")
        if scenario["equipment"]:
            vehicle = scenario["equipment"][0]
            vehicle["status"] = "unavailable"
            self.assertEqual(studio.scenario_package(scenario)["equipment"][0]["status"], "unavailable")
        if scenario["turnaround_requirements"]:
            before = studio.scenario_package(scenario)["turnaround_requirements"][0]["duration_minutes"]
            scenario["turnaround_requirements"][0]["duration_minutes"] = before + 1
            self.assertEqual(studio.scenario_package(scenario)["turnaround_requirements"][0]["duration_minutes"], before + 1)

    def test_map_selected_closure_generates_native_scenario(self):
        scenario = studio.new_scenario("kauo")
        scenario["disruptions"] = [{"disruption_id": "STUDIO-CLOSURE-A", "type": "route_closure",
                                     "resource_id": "KAUO-TWY-A-RAMP-CONNECTOR",
                                     "start_time": "2026-10-09T14:00:15Z", "end_time": "2026-10-09T14:45:00Z"}]
        self.assertTrue(studio.validate_scenario(scenario)["valid"])
        with tempfile.TemporaryDirectory(prefix="ramplab-studio-generate-") as temp:
            root = Path(temp)
            package_file = root / "canonical.json"
            package_file.write_text(json.dumps(studio.scenario_package(scenario)), encoding="utf-8")
            output = root / "generated"
            result = generate(package_file, studio._mapping("kauo"), output, seed=42)
            checked = validate_generated(output)
            native = json.loads((output / "scenario.json").read_text(encoding="utf-8"))
            self.assertTrue(checked["valid"])
            self.assertEqual(result["scenario"]["road_events"][0]["edge"], "twy_a_mid_fbo_primary")
            self.assertEqual(native["road_events"][0]["edge"], "twy_a_mid_fbo_primary")

    def test_save_load_preserves_versioned_scenario(self):
        scenario = studio.new_scenario("kauo")
        with tempfile.TemporaryDirectory(prefix="ramplab-studio-store-") as temp, patch.object(studio, "STORE", Path(temp)):
            Path(temp).mkdir(exist_ok=True)
            store = studio.ScenarioStore()
            saved = store.save(scenario)
            reopened = store.get(saved["id"])
            self.assertEqual(reopened["schema_version"], 1)
            self.assertEqual(reopened["dataset_id"], scenario["dataset_id"])
            self.assertEqual(store.list()[0]["id"], saved["id"])

    def test_duplicate_run_protection_and_async_orchestration(self):
        scenario = studio.new_scenario("kauo")
        with tempfile.TemporaryDirectory(prefix="ramplab-studio-runs-") as temp, patch.object(studio, "RUNS", Path(temp)):
            manager = studio.RunManager()
            manager._executor = _QueuedExecutor()
            first = manager.launch(scenario)
            self.assertEqual(first["status"], "queued")
            self.assertEqual(len(manager._executor.calls), 1)
            with self.assertRaisesRegex(ValueError, "already running"):
                manager.launch(deepcopy(scenario))
            self.assertEqual(manager.get(first["id"])["status"], "queued")

    def test_unreal_viewer_receives_the_generated_scenario_file(self):
        with tempfile.TemporaryDirectory(prefix="ramplab-studio-viewer-") as temp:
            root = Path(temp)
            scenario = root / "generated" / "scenario.json"
            scenario.parent.mkdir()
            scenario.write_text("{}", encoding="utf-8")
            run = {"id": "a" * 32, "status": "complete", "folder": str(root)}
            with patch.object(studio.RunManagerSingleton, "get", return_value=run), \
                 patch("tools.release.ramplab.launch_unreal", return_value=31415) as launch:
                result = studio.launch_viewer(run["id"])
            self.assertEqual(result["pid"], 31415)
            self.assertEqual(result["message"], "The Goal 29 viewer was asked to open this generated scenario.")
            self.assertEqual(launch.call_args.kwargs["scenario_file"], scenario)


if __name__ == "__main__":
    unittest.main()
