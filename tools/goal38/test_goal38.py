from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from tools.goal38.run_matrix import POLICIES, configure_scenario


class Goal38MatrixTests(unittest.TestCase):
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


if __name__ == "__main__":
    unittest.main()
