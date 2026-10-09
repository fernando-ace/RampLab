from __future__ import annotations

import copy
import json
from pathlib import Path
import tempfile
import unittest
from contextlib import redirect_stdout
from io import StringIO

from tools.airport_data_ingestion.core import canonical_bytes, load_dataset
from .cli import main
from .generator import GenerationError, generate, inspect_generated, load_mapping, load_package, validate_generated

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / "tools" / "airport_data_ingestion" / "examples" / "synthetic" / "manifest.json"
MAPPING = ROOT / "tools" / "airport_scenario_generation" / "mapping.synthetic.json"


class AirportScenarioGenerationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.package_data, _, _ = load_dataset(FIXTURE)
        self.package = self.root / "canonical.json"
        self.package.write_bytes(canonical_bytes(self.package_data))
        self.mapping_data = json.loads(MAPPING.read_text(encoding="utf-8"))
        self.mapping = self.root / "mapping.json"
        self.mapping.write_text(json.dumps(self.mapping_data, indent=2) + "\n", encoding="utf-8")
        geometry = MAPPING.parent / self.mapping_data["geometry_file"]
        (self.root / geometry.name).write_bytes(geometry.read_bytes())

    def tearDown(self):
        self.temp.cleanup()

    def write_package(self, value):
        self.package.write_bytes(canonical_bytes(value))

    def write_mapping(self, value):
        self.mapping.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")

    def test_canonical_package_load_and_schema_rejection(self):
        data, raw = load_package(self.package)
        self.assertEqual(data["schema_version"], "1.0")
        self.assertTrue(raw)
        invalid = copy.deepcopy(data)
        invalid["schema_version"] = "2.0"
        self.write_package(invalid)
        with self.assertRaisesRegex(GenerationError, "unsupported canonical major"):
            load_package(self.package)

    def test_invalid_validation_and_missing_structure_are_rejected(self):
        invalid = copy.deepcopy(self.package_data)
        invalid["validation"]["status"] = "error"
        self.write_package(invalid)
        with self.assertRaisesRegex(GenerationError, "blocking errors"):
            load_package(self.package)
        invalid = copy.deepcopy(self.package_data)
        del invalid["sources"]
        self.write_package(invalid)
        with self.assertRaisesRegex(GenerationError, "missing required sections"):
            load_package(self.package)

    def test_mapping_validation_duplicate_unknown_and_malformed(self):
        mapping, _, _, _ = load_mapping(self.mapping)
        self.assertEqual(mapping["gate_ids"]["A1"], "A1")
        invalid = copy.deepcopy(mapping)
        invalid["gate_ids"]["A2"] = "missing_gate"
        self.write_mapping(invalid)
        with self.assertRaisesRegex(GenerationError, "unknown simulator gate"):
            load_mapping(self.mapping)
        invalid = copy.deepcopy(mapping)
        invalid["gate_ids"]["A2"] = "A1"
        self.write_mapping(invalid)
        with self.assertRaisesRegex(GenerationError, "duplicate gate resource"):
            load_mapping(self.mapping)
        self.mapping.write_text("{broken", encoding="utf-8")
        with self.assertRaises(GenerationError):
            load_mapping(self.mapping)

    def test_missing_gate_mapping_and_unsupported_required_service_block(self):
        mapping = copy.deepcopy(self.mapping_data)
        del mapping["gate_ids"]["R1"]
        self.write_mapping(mapping)
        with self.assertRaisesRegex(GenerationError, "missing required gate mapping"):
            generate(self.package, self.mapping, self.root / "missing-gate")
        self.write_mapping(self.mapping_data)
        package = copy.deepcopy(self.package_data)
        package["turnaround_requirements"][0]["service_type"] = "unsupported_service"
        self.write_package(package)
        with self.assertRaisesRegex(GenerationError, "no RampLab equivalent"):
            generate(self.package, self.mapping, self.root / "missing-service")

    def test_gate_route_equipment_and_turnaround_mappings(self):
        output = self.root / "generated"
        result = generate(self.package, self.mapping, output)
        scenario = result["scenario"]
        self.assertEqual({x["gate"] for x in scenario["aircraft"]}, {"A1", "A2"})
        self.assertEqual({x["type"] for x in scenario["fleet"]["vehicles"]}, {"fueling", "baggage", "catering"})
        vehicle_spawns = {x["id"]: x["depot_node"] for x in scenario["fleet"]["vehicles"]}
        self.assertEqual(vehicle_spawns, {"FUEL-1": "depot", "BAG-1": "south", "CAT-1": "gate_a3"})
        self.assertEqual(len(set(vehicle_spawns.values())), len(vehicle_spawns))
        self.assertEqual(sum(len(x["service_tasks"]) for x in scenario["aircraft"]), 4)
        self.assertEqual(scenario["road_events"][0]["edge"], "north_south_taxi")
        self.assertEqual(len(scenario["road_events"]), 2)
        self.assertEqual(scenario["road_events"][0]["enabled"], False)
        self.assertEqual(scenario["road_events"][1]["enabled"], True)

    def test_duplicate_active_vehicle_spawn_node_is_rejected(self):
        mapping = copy.deepcopy(self.mapping_data)
        mapping["equipment_types"]["catering_vehicle"]["initial_node"] = "depot"
        self.write_mapping(mapping)
        with self.assertRaisesRegex(GenerationError, "shares simulator spawn node 'depot'"):
            generate(self.package, self.mapping, self.root / "duplicate-spawn")

    def test_fuel_only_airport_does_not_require_synthetic_baggage_fleet(self):
        package = copy.deepcopy(self.package_data)
        package["turnaround_requirements"] = [
            row for row in package["turnaround_requirements"] if row["service_type"] == "fueling"
        ]
        package["equipment"] = [row for row in package["equipment"] if row["type"] == "fuel_vehicle"]
        self.write_package(package)
        mapping = copy.deepcopy(self.mapping_data)
        mapping["equipment_types"] = {"fuel_vehicle": mapping["equipment_types"]["fuel_vehicle"]}
        self.write_mapping(mapping)
        result = generate(self.package, self.mapping, self.root / "fuel-only")
        self.assertEqual([vehicle["type"] for vehicle in result["scenario"]["fleet"]["vehicles"]], ["fueling"])

    def test_unsupported_metadata_and_outage_warning_are_explicit(self):
        result = generate(self.package, self.mapping, self.root / "generated")
        warnings = result["manifest"]["warnings"]
        self.assertTrue(any("EQUIPMENT_METADATA_ONLY" in warning for warning in warnings))
        self.assertTrue(any("UNSUPPORTED_FINITE_EQUIPMENT_OUTAGE" in warning for warning in warnings))

    def test_identity_map_and_provenance_hashes(self):
        result = generate(self.package, self.mapping, self.root / "generated")
        manifest = result["manifest"]
        identity = json.loads((self.root / "generated" / "identity-map.json").read_text(encoding="utf-8"))
        self.assertEqual(len(identity["flights"]), 4)
        self.assertIn("SYN101", identity["flights"])
        self.assertIn("N101RL", identity["aircraft"])
        self.assertEqual(manifest["source_canonical_sha256"], __import__("hashlib").sha256(self.package.read_bytes()).hexdigest())
        self.assertEqual(manifest["mapping_configuration_sha256"], __import__("hashlib").sha256(self.mapping.read_bytes()).hexdigest())
        self.assertEqual(len(manifest["source_provenance"]), 6)

    def test_generation_is_byte_identical_and_seed_changes_scenario(self):
        left, right = self.root / "left", self.root / "right"
        generate(self.package, self.mapping, left, seed=42)
        generate(self.package, self.mapping, right, seed=42)
        for name in ("scenario.json", "manifest.json", "identity-map.json", "support-matrix.json"):
            self.assertEqual((left / name).read_bytes(), (right / name).read_bytes())
        other = self.root / "other-seed"
        generate(self.package, self.mapping, other, seed=43)
        self.assertNotEqual((left / "scenario.json").read_bytes(), (other / "scenario.json").read_bytes())

    def test_utc_epoch_and_generation_without_disruptions(self):
        output = self.root / "control"
        result = generate(self.package, self.mapping, output, without_disruptions=True)
        self.assertEqual(result["manifest"]["scenario_epoch_utc"], "2026-10-07T13:00:00Z")
        self.assertEqual(result["manifest"]["generated_disruptions"]["route_closures"], 0)
        self.assertNotIn("road_events", result["scenario"])

    def test_control_and_disruption_variants_keep_epoch_when_disruption_is_earlier(self):
        package = copy.deepcopy(self.package_data)
        closure = next(item for item in package["disruptions"] if item["type"] == "route_closure")
        closure["start_time"] = "2026-10-07T12:00:00Z"
        closure["end_time"] = "2026-10-07T12:30:00Z"
        self.write_package(package)

        disruption = generate(self.package, self.mapping, self.root / "disruption")
        control = generate(self.package, self.mapping, self.root / "control", without_disruptions=True)

        self.assertEqual(disruption["manifest"]["scenario_epoch_utc"], control["manifest"]["scenario_epoch_utc"])
        self.assertEqual(disruption["scenario"]["aircraft"], control["scenario"]["aircraft"])
        self.assertEqual(disruption["scenario"]["road_events"][0]["time_seconds"], 0)
        self.assertNotIn("road_events", control["scenario"])

    def test_flight_delay_and_delayed_service_use_native_schedule_and_task_events(self):
        package = copy.deepcopy(self.package_data)
        package["disruptions"].extend([
            {"disruption_id": "DIS-FLIGHT", "type": "flight_delay", "flight_id": "SYN202",
             "delay_minutes": 5, "start_time": "2026-10-07T14:50:00Z"},
            {"disruption_id": "DIS-SERVICE", "type": "delayed_service", "requirement_id": "REQ-1",
             "delay_minutes": 3, "start_time": "2026-10-07T13:00:00Z"},
        ])
        self.write_package(package)
        result = generate(self.package, self.mapping, self.root / "delayed")
        by_id = {item["id"]: item for item in result["scenario"]["aircraft"]}
        self.assertEqual(by_id["N102RL"]["scheduled_departure_seconds"], 9300)
        self.assertEqual(result["scenario"]["turnaround_disruptions"][0]["duration_seconds"], 1380)

    def test_generated_validation_hashes_and_inspect_command(self):
        output = self.root / "generated"
        generate(self.package, self.mapping, output)
        self.assertTrue(validate_generated(output)["valid"])
        inspected = inspect_generated(output)
        self.assertEqual(inspected["flights_mapped"], 4)
        self.assertEqual(inspected["vehicles_generated"], 3)
        with redirect_stdout(StringIO()) as stdout:
            self.assertEqual(main(["inspect", str(output)]), 0)
        self.assertIn("scenario_sha256", stdout.getvalue())
        manifest_path = output / "manifest.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        manifest["output_file_sha256"]["scenario.json"] = "0" * 64
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
        with self.assertRaisesRegex(GenerationError, "hash mismatch"):
            validate_generated(output)


if __name__ == "__main__":
    unittest.main()
