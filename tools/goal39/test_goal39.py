from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from tools.goal39.run_matrix import POLICIES, compare_outcome, configure_scenario


class Goal39MatrixTests(unittest.TestCase):
    def test_all_policies_are_written_as_a_single_scenario_delta(self):
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

    def test_counterfactual_is_included_without_changing_scenario_inputs(self):
        self.assertIn("COUNTERFACTUAL", POLICIES)
        self.assertEqual(len(POLICIES), 5)

    def test_matched_run_outcome_comparison_uses_lexicographic_kpis(self):
        def row(completed: int, failed: int, wait: int, taxi: int, distance: int) -> dict:
            return {"metrics": {"agent_objectives_completed": completed, "agent_objectives_failed": failed,
                "surface": {"wait_seconds": wait, "taxi_seconds": taxi, "taxi_distance_m": distance}}}
        self.assertEqual(compare_outcome(row(3, 0, 100, 200, 300), row(2, 0, 0, 100, 100)), "improved")
        self.assertEqual(compare_outcome(row(3, 0, 101, 200, 300), row(3, 0, 100, 200, 300)), "worsened")
        self.assertEqual(compare_outcome(row(3, 0, 100, 200, 300), row(3, 0, 100, 200, 300)), "no_effect")


if __name__ == "__main__":
    unittest.main()
