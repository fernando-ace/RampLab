"""Goal 25 KAUO package, topology, and deterministic generation checks."""
import json
import tempfile
import unittest
from pathlib import Path

from tools.airport_data_ingestion.core import load_dataset, canonical_bytes
from tools.airport_scenario_generation.generator import generate

ROOT = Path(__file__).resolve().parents[2]
PACKAGE_MANIFEST = ROOT / "tools/airport_data_ingestion/examples/kauo/manifest.json"
MAPPING = ROOT / "tools/airport_scenario_generation/mapping.kauo.json"


class KauoGoal25Tests(unittest.TestCase):
    def test_canonical_input_valid_and_explicitly_illustrative(self):
        data, findings, _ = load_dataset(PACKAGE_MANIFEST)
        self.assertEqual(data["airport"]["airport_id"], "KAUO")
        self.assertEqual(data["validation"]["status"], "valid")
        self.assertFalse([item for item in findings if item["severity"] == "error"])
        self.assertEqual(len(data["flights"]), 2)
        self.assertEqual({item["operation"] for item in data["flights"]}, {"arrival", "departure"})
        self.assertIn("illustrative", data["dataset"]["description"].lower())

    def test_generated_pair_connectivity_spawn_safety_and_disruption(self):
        data, findings, _ = load_dataset(PACKAGE_MANIFEST)
        self.assertFalse([item for item in findings if item["severity"] == "error"])
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            canonical = root / "canonical.json"
            canonical.write_bytes(canonical_bytes(data))
            control = generate(canonical, MAPPING, root / "control", seed=42, without_disruptions=True)
            disrupted = generate(canonical, MAPPING, root / "disrupted", seed=42)
            repeat = generate(canonical, MAPPING, root / "repeat", seed=42, without_disruptions=True)

            scenario = control["scenario"]
            node_ids = {node["id"] for node in scenario["airport"]["nodes"]}
            edge_pairs = [(edge["from"], edge["to"]) for edge in scenario["airport"]["edges"]]
            gate_nodes = {gate["node"] for gate in scenario["gates"]}
            vehicle_nodes = {vehicle["depot_node"] for vehicle in scenario["fleet"]["vehicles"]}
            self.assertTrue(gate_nodes <= node_ids)
            self.assertEqual(vehicle_nodes, set())
            self.assertEqual(len(gate_nodes), 1)
            # The illustrative parking point connects into the airport graph.
            adjacency = {node: set() for node in node_ids}
            for left, right in edge_pairs:
                adjacency[left].add(right)
                adjacency[right].add(left)
            visited = set()
            pending = [next(iter(gate_nodes))]
            while pending:
                current = pending.pop()
                if current in visited:
                    continue
                visited.add(current)
                pending.extend(adjacency[current] - visited)
            self.assertTrue(gate_nodes <= visited)
            self.assertEqual(scenario["aircraft"][0]["operation_type"], "arrival_turnaround")
            self.assertEqual(scenario["aircraft"][0]["service_tasks"][0]["type"], "pushback_preparation")

            self.assertEqual(scenario["surface_operations"]["aircraft_speed_mps"], 5.0)
            self.assertNotIn("road_events", scenario)
            self.assertEqual(disrupted["scenario"]["road_events"][0]["edge"], "twy_a_mid_fbo_primary")
            self.assertEqual(disrupted["scenario"]["road_events"][0]["enabled"], False)
            surface = scenario["surface_operations"]
            self.assertIn(surface["arrival_exit"], visited)
            self.assertIn(surface["departure_handoff"], visited)
            self.assertIn(surface["runway_node"], visited)
            # The mapped closure must still leave an arrival-exit-to-stand path.
            closed_adjacency = {node: set() for node in node_ids}
            closed_edge = disrupted["scenario"]["road_events"][0]["edge"]
            for edge in scenario["airport"]["edges"]:
                if edge["id"] == closed_edge:
                    continue
                closed_adjacency[edge["from"]].add(edge["to"])
                closed_adjacency[edge["to"]].add(edge["from"])
            reachable = set()
            pending = [surface["arrival_exit"]]
            while pending:
                current = pending.pop()
                if current in reachable:
                    continue
                reachable.add(current)
                pending.extend(closed_adjacency[current] - reachable)
            self.assertTrue(gate_nodes <= reachable)
            self.assertEqual(scenario["fleet"]["vehicles"], [])
            for name in ("scenario.json", "manifest.json", "identity-map.json", "support-matrix.json"):
                self.assertEqual((root / "control" / name).read_bytes(), (root / "repeat" / name).read_bytes())


if __name__ == "__main__":
    unittest.main()
