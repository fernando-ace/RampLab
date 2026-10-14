"""Runtime-aligned route validation over the committed Goal 44 control graph."""
from __future__ import annotations

import unittest
from pathlib import Path

from tools.airport_scenario_generation.audit_goal44_topology import audit

ROOT = Path(__file__).resolve().parents[2]


class Goal44TopologyAuditTests(unittest.TestCase):
    def test_real_control_graph_has_safe_arrival_and_departure_paths(self):
        report = audit(ROOT / "docs/goal44-evidence/generated/control/scenario.json")
        self.assertEqual(report["fleet_start_position_count"], 9)
        self.assertTrue(report["clearance_pass"])
        self.assertTrue(report["all_arrivals_and_departures_reachable"])
        self.assertEqual(len(report["flight_routes"]), 3)
        for flight in report["flight_routes"]:
            with self.subTest(flight=flight["aircraft_id"]):
                arrival = flight["arrival_to_gate"]
                departure = flight["gate_to_departure_handoff"]
                self.assertIsNotNone(arrival)
                self.assertIsNotNone(departure)
                self.assertGreater(arrival["distance_m"], 0.0)
                self.assertGreater(departure["distance_m"], 0.0)
                self.assertGreaterEqual(arrival["minimum_clearance_to_idle_fleet_m"], 30.0)
                self.assertGreaterEqual(departure["minimum_clearance_to_idle_fleet_m"], 30.0)
        self.assertEqual(report["clearance_rejected_edges"], [])

    def test_disruption_closure_alternates_are_flagged_as_unverified(self):
        report = audit(ROOT / "docs/goal44-evidence/generated/disruption/scenario.json")
        self.assertEqual(report["scheduled_closed_edges"], ["twy_a_mid_fbo_primary"])
        self.assertTrue(report["all_arrivals_reachable_in_static_graph_during_scheduled_closure"])
        for flight in report["flight_routes"]:
            route = flight["arrival_to_gate_while_scheduled_edges_closed"]
            self.assertIsNotNone(route)
            self.assertTrue(route["crosses_runway_inventory"])
            self.assertTrue(route["contains_unverified_runway_crossing"])


if __name__ == "__main__":
    unittest.main()
