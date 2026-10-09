"""KAUO FAA-anchor calibration and scenario-generation checks for Goal 26."""
import json
import shutil
import tempfile
import unittest
from pathlib import Path

from tools.airport_data_ingestion.core import canonical_bytes, load_dataset
from tools.airport_scenario_generation.calibrate_kauo import calibrate
from tools.airport_scenario_generation.generator import generate

ROOT = Path(__file__).resolve().parents[2]
PACKAGE_MANIFEST = ROOT / "tools/airport_data_ingestion/examples/kauo/manifest.json"
CALIBRATION_DIRECTORY = ROOT / "tools/airport_scenario_generation"
ANCHORS = CALIBRATION_DIRECTORY / "kauo.anchors.json"
GEOMETRY = CALIBRATION_DIRECTORY / "kauo.geometry.json"
MAPPING = CALIBRATION_DIRECTORY / "mapping.kauo.json"


class KauoGoal26Tests(unittest.TestCase):
    def test_authoritative_threshold_errors_and_reported_baseline_miss(self):
        report = json.loads((CALIBRATION_DIRECTORY / "kauo_alignment_validation.json").read_text(encoding="utf-8"))
        self.assertEqual(len(report["authoritative_runway_thresholds"]), 4)
        self.assertEqual(len(report["generated_runway_geometry_endpoint_errors"]), 4)
        self.assertTrue(all(row["classification"] == "AUTHORITATIVE" for row in report["authoritative_runway_thresholds"]))
        self.assertTrue(all(row["status"] == "PASS" for row in report["authoritative_runway_thresholds"]))
        self.assertTrue(all(row["status"] == "PASS" for row in report["generated_runway_geometry_endpoint_errors"]))
        self.assertLess(report["registration_maximum_threshold_error_m"], 0.05)
        self.assertGreater(report["baseline_maximum_threshold_error_m"], 60.0)
        self.assertAlmostEqual(report["baseline_maximum_threshold_error_m"], 66.832, places=3)

    def test_geometry_rebuild_is_byte_deterministic(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            geometry = root / "kauo.geometry.json"
            report = root / "kauo_alignment_validation.json"
            shutil.copyfile(GEOMETRY, geometry)
            calibrate(ANCHORS, geometry, report)
            first_geometry, first_report = geometry.read_bytes(), report.read_bytes()
            calibrate(ANCHORS, geometry, report)
            self.assertEqual(geometry.read_bytes(), first_geometry)
            self.assertEqual(report.read_bytes(), first_report)

    def test_generated_scenario_contains_authoritative_geometry_deterministically(self):
        data, findings, _ = load_dataset(PACKAGE_MANIFEST)
        self.assertFalse([finding for finding in findings if finding["severity"] == "error"])
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            canonical = root / "canonical.json"
            canonical.write_bytes(canonical_bytes(data))
            first = generate(canonical, MAPPING, root / "first", seed=42, without_disruptions=True)
            second = generate(canonical, MAPPING, root / "second", seed=42, without_disruptions=True)
            first_bytes = (root / "first" / "scenario.json").read_bytes()
            self.assertEqual(first_bytes, (root / "second" / "scenario.json").read_bytes())
            scenario = first["scenario"]
            self.assertEqual(len(scenario["airport"]["features"]["runways"]), 2)
            self.assertIn("runway_intersection", {node["id"] for node in scenario["airport"]["nodes"]})
            self.assertTrue(scenario["airport"]["calibration"]["scenario_axis_convention"].startswith("+X east, +Y north"))


if __name__ == "__main__":
    unittest.main()
