"""Goal 44 movement semantics and stationary-GSE clearance checks."""
from __future__ import annotations

import math
import unittest

from tools.airport_scenario_generation.goal44_geometry import build_geometry


def _distance_to_line(point: tuple[float, float], line: list[dict[str, float]]) -> float:
    distances = []
    for left, right in zip(line, line[1:]):
        dx, dy = right["x_m"] - left["x_m"], right["y_m"] - left["y_m"]
        denominator = dx * dx + dy * dy
        fraction = 0.0 if denominator == 0 else max(0.0, min(1.0,
            ((point[0] - left["x_m"]) * dx + (point[1] - left["y_m"]) * dy) / denominator))
        distances.append(math.hypot(point[0] - left["x_m"] - fraction * dx,
                                    point[1] - left["y_m"] - fraction * dy))
    return min(distances, default=math.inf)


class Goal44GeometryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.airport = build_geometry()["airport"]

    def test_service_and_gse_edges_are_never_aircraft_shortcuts(self):
        edges = self.airport["edges"]
        service_edges = [edge for edge in edges if edge.get("classification", "").startswith((
            "APPROXIMATE GSE", "ASSUMED FOR SIMULATION GSE", "ASSUMED FOR SIMULATION shared stand",
            "ASSUMED FOR SIMULATION short service"))]
        self.assertGreater(len(service_edges), 70)
        self.assertTrue(all(not edge["aircraft_allowed"] for edge in service_edges))
        self.assertTrue(all(edge["movement_class"] == "GROUND_VEHICLE_SERVICE_ONLY"
                            for edge in service_edges))
        self.assertEqual(self.airport["surface_routing_model"]["route_counts"]["ground_only_by_semantics"],
                         len(service_edges))

    def test_every_aircraft_edge_clears_attached_gse_depots_by_30m(self):
        depots = [(node["x_m"], node["y_m"]) for node in self.airport["nodes"]
                  if str(node.get("classification", "")).startswith("APPROXIMATE GSE depot")]
        self.assertEqual(len(depots), 9)
        aircraft_edges = [edge for edge in self.airport["edges"] if edge["aircraft_allowed"]]
        self.assertEqual(len(aircraft_edges), 10)
        minimum = min(_distance_to_line(depot, edge["surface_centerline"])
                      for edge in aircraft_edges for depot in depots)
        self.assertGreaterEqual(minimum, 30.0)

    def test_runway_crossing_ground_connectors_remain_unavailable(self):
        connectors = [edge for edge in self.airport["edges"]
                      if edge.get("ground_vehicle_access_limitation")]
        self.assertEqual(len(connectors), 5)
        self.assertTrue(all(not edge["ground_vehicle_allowed"] and edge["aircraft_allowed"]
                            for edge in connectors))
        self.assertTrue(all("runway-crossing permission is unverified" in edge["ground_vehicle_access_limitation"]
                            for edge in connectors))


if __name__ == "__main__":
    unittest.main()
