"""Goal 27 KAUO operational vertical source and generation contracts."""
import json
import tempfile
import unittest
from pathlib import Path

from tools.airport_data_ingestion.core import canonical_bytes, load_dataset
from tools.airport_scenario_generation.generator import generate

ROOT = Path(__file__).resolve().parents[2]
DATA = ROOT / "tools/airport_data_ingestion/examples/kauo_goal27"
MAPPING = ROOT / "tools/airport_scenario_generation/mapping.kauo_goal27.json"


class KauoGoal27Tests(unittest.TestCase):
    def test_source_dataset_has_two_turnarounds_three_vehicles_and_explicit_assumptions(self):
        data, findings, counts = load_dataset(DATA / "manifest.json")
        self.assertFalse([finding for finding in findings if finding["severity"] == "error"])
        self.assertEqual(data["airport"]["airport_id"], "KAUO")
        self.assertEqual(counts["aircraft"], 2)
        self.assertEqual(counts["flights"], 4)
        self.assertEqual(counts["gates"], 2)
        self.assertEqual(counts["equipment"], 3)
        self.assertEqual(counts["turnaround_requirements"], 8)
        self.assertTrue(all(row["classification"].startswith("ASSUMED") for row in data["equipment"]))
        self.assertTrue(all(row["classification"].startswith("ASSUMED") for row in data["flights"]))
        self.assertTrue(all(row["classification"].startswith("ASSUMED") for row in data["turnaround_requirements"]))

    def test_generated_control_disruption_and_repeat_preserve_runway_registration(self):
        data, findings, _ = load_dataset(DATA / "manifest.json")
        self.assertFalse([finding for finding in findings if finding["severity"] == "error"])
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            package = root / "canonical.json"
            package.write_bytes(canonical_bytes(data))
            control = generate(package, MAPPING, root / "control", seed=42, without_disruptions=True)
            disruption = generate(package, MAPPING, root / "disruption", seed=42)
            repeated = generate(package, MAPPING, root / "repeat", seed=42, without_disruptions=True)

            scenario = control["scenario"]
            nodes = {item["id"] for item in scenario["airport"]["nodes"]}
            self.assertEqual(len(scenario["aircraft"]), 2)
            self.assertEqual(len(scenario["fleet"]["vehicles"]), 3)
            self.assertEqual(len(scenario["gates"]), 2)
            self.assertTrue(scenario["surface_operations"]["return_service_vehicles_to_depot_after_task"])
            self.assertTrue({"ga_parking_stand_02", "fbo_service_staging_west", "fbo_service_staging_east"} <= nodes)
            self.assertTrue(all(item["operation_type"] == "arrival_turnaround" for item in scenario["aircraft"]))
            self.assertTrue(all(len(item["service_tasks"]) == 4 for item in scenario["aircraft"]))
            tasks = {task["id"]: task for aircraft in scenario["aircraft"] for task in aircraft["service_tasks"]}
            for aircraft_id in ("KAUO-SIM-GA-01", "KAUO-SIM-GA-02"):
                prefix = f"KAUO-GA{aircraft_id[-2:]}"
                unload = f"{prefix}-UNLOAD"
                fuel = f"{prefix}-FUEL"
                load = f"{prefix}-LOAD"
                push = f"{prefix}-PUSH"
                self.assertEqual(tasks[unload]["prerequisites"], [fuel])
                self.assertEqual(tasks[load]["prerequisites"], [unload])
                self.assertEqual(tasks[push]["prerequisites"], [load])
            self.assertEqual(control["scenario"]["airport"]["calibration"]["scenario_axis_convention"],
                             "+X east, +Y north, meters; these are airport-local ENU coordinates.")
            self.assertNotIn("road_events", control["scenario"])
            self.assertEqual(disruption["scenario"]["road_events"][0]["edge"], "twy_a_mid_fbo_primary")
            self.assertEqual(disruption["scenario"]["road_events"][0]["enabled"], False)
            self.assertEqual(disruption["scenario"]["road_events"][1]["enabled"], True)
            for filename in ("scenario.json", "manifest.json", "identity-map.json", "support-matrix.json"):
                self.assertEqual((root / "control" / filename).read_bytes(),
                                 (root / "repeat" / filename).read_bytes())
            geometry = json.loads((ROOT / "tools/airport_scenario_generation/kauo_goal27.geometry.json").read_text(encoding="utf-8"))
            runway_map = {item["identifier"]: item for item in geometry["airport"]["features"]["runways"]}
            self.assertEqual(runway_map["18/36"]["source_classification"], "AUTHORITATIVE endpoints / DERIVED centerline and heading")
            self.assertAlmostEqual(runway_map["18/36"]["length_m"], 1604.397886, places=6)


if __name__ == "__main__":
    unittest.main()
