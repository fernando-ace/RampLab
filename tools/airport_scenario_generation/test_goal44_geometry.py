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

    def test_service_point_attachments_preserve_fleet_vehicle_clearance(self):
        service_points = [node for node in self.airport["nodes"]
                          if node["id"].endswith("_service_point")]
        self.assertEqual(len(service_points), 27)
        points = [(node["x_m"], node["y_m"]) for node in service_points]
        minimum = min(math.dist(left, right) for index, left in enumerate(points)
                      for right in points[index + 1:])
        routing = self.airport["surface_routing_model"]
        summary = routing["attachment_summary"]
        self.assertGreaterEqual(minimum, 4.0)
        self.assertEqual(summary["gse_service_point_count"], len(service_points))
        self.assertEqual(summary["gse_service_point_required_separation_m"], 4.0)
        self.assertEqual(summary["gse_service_approach_required_separation_m"], 6.0)
        self.assertEqual(summary["gse_service_point_minimum_separation_m"], round(minimum, 3))
        self.assertTrue(all(node["surface_attachment"]["source_feature_id"] for node in service_points))
        approaches = [(node["x_m"], node["y_m"]) for node in self.airport["nodes"]
                      if node["id"].endswith("_service_approach")]
        self.assertTrue(all(min(math.dist(point, approach) for approach in approaches) >= 6.0
                            for point in points))

    def test_runway_crossing_ground_connectors_remain_unavailable(self):
        connectors = [edge for edge in self.airport["edges"]
                      if edge.get("ground_vehicle_access_limitation")]
        self.assertEqual(len(connectors), 5)
        self.assertTrue(all(not edge["ground_vehicle_allowed"] and edge["aircraft_allowed"]
                            for edge in connectors))
        self.assertTrue(all("runway-crossing permission is unverified" in edge["ground_vehicle_access_limitation"]
                            for edge in connectors))

    def test_large_node_snaps_are_reported_as_unverified_not_manually_reconciled(self):
        routing = self.airport["surface_routing_model"]
        attachments = routing["node_attachments"]
        summary = routing["attachment_summary"]
        unverified = [item for item in attachments
                      if item["classification"] == "UNVERIFIED_APPROXIMATE_NODE_SNAP"]
        self.assertTrue(unverified)
        self.assertTrue(all(item["attachment_displacement_m"] > routing["grid_size_m"]
                            for item in unverified))
        self.assertTrue(all(item["node_location_status"] for item in unverified))
        self.assertEqual(summary["node_count"], len(attachments))
        self.assertEqual(summary["unverified_approximate_node_snap_count"], len(unverified))
        self.assertEqual(summary["maximum_displacement_m"], max(
            item["attachment_displacement_m"] for item in attachments))
        self.assertNotIn("MANUALLY_RECONCILED", routing["classification"])


if __name__ == "__main__":
    unittest.main()
