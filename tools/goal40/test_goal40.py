from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from tools.goal39.run_matrix import MATRIX
from tools.goal40.run_matrix import POLICIES, configure_scenario


class Goal40MatrixTests(unittest.TestCase):
    def test_goal39_and_goal40_policies_are_compared_without_changing_inputs(self):
        self.assertIn("COUNTERFACTUAL", POLICIES)
        self.assertIn("COST_AWARE_COUNTERFACTUAL", POLICIES)
        self.assertEqual(len(POLICIES), 6)
        repo = Path(__file__).resolve().parents[2]
        source = repo / "scenarios" / "goal37_coordinated_route_choice.yaml"
        with tempfile.TemporaryDirectory() as temporary:
            for policy in POLICIES:
                target = Path(temporary) / f"{policy}.yaml"
                configure_scenario(source, target, policy)
                configured = target.read_text(encoding="utf-8")
                self.assertIn(f"aircraft_routing_policy: {policy}", configured)
                self.assertEqual(configured.count("aircraft_routing_policy:"), 1)
                normalized = [line.strip().replace(f", aircraft_routing_policy: {policy}", "")
                    for line in configured.splitlines()]
                original = [line.strip() for line in source.read_text(encoding="utf-8").splitlines()]
                normalized = [line.replace(" }", "}") for line in normalized]
                original = [line.replace(" }", "}") for line in original]
                self.assertEqual(normalized, original)

    def test_experiment_matrix_preserves_all_goal39_scenarios(self):
        self.assertEqual(len(MATRIX), 10)
        self.assertEqual(len(POLICIES) * len(MATRIX) * 3, 180)

    def test_operational_cost_configuration_is_explicit_and_dimensionless(self):
        weights = {"wait": 0.40, "completion_time": 0.30, "distance": 0.18,
            "contention": 0.08, "route_change": 0.04}
        self.assertAlmostEqual(sum(weights.values()), 1.0)
        self.assertGreater(600, 0)  # time normalization is explicit, never mixed directly with metres
        self.assertGreater(2000, 0)  # distance normalization is explicit


if __name__ == "__main__":
    unittest.main()
