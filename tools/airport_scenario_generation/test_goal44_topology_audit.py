"""Runtime-aligned route validation over the committed Goal 44 control graph."""
from __future__ import annotations

import unittest

from tools.airport_scenario_generation.audit_goal44_topology import audit


class Goal44TopologyAuditTests(unittest.TestCase):
    def test_real_control_graph_has_safe_arrival_and_departure_paths(self):
        report = audit()
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


if __name__ == "__main__":
    unittest.main()
