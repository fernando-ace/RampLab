from __future__ import annotations

import json
from pathlib import Path
import tempfile
import unittest

from .goal42_replay import unreal_replay_rows, write_unreal_replay
from .build_goal42_correlation import correlate


class Goal42ReplayTests(unittest.TestCase):
    def _events(self, rows: list[dict]) -> Path:
        self.temp = tempfile.TemporaryDirectory()
        path = Path(self.temp.name) / "events.jsonl"
        path.write_text("\n".join(json.dumps(row) for row in rows) + "\n", encoding="utf-8")
        self.addCleanup(self.temp.cleanup)
        return path

    def test_preserves_identity_time_and_route_and_transforms_enu_to_unreal(self) -> None:
        source = self._events([{"sequence": 4, "time_seconds": 8, "type": "SurfaceReservationAcquired",
            "entity_id": "aircraft-3", "entity_type": "aircraft", "edge_id": 12,
            "position_m": {"x": 12.5, "y": -8}, "heading_rad": 0.5}])
        rows, counts = unreal_replay_rows(source)
        self.assertEqual(counts, {"events": 1, "with_pose": 1, "without_pose": 0})
        self.assertEqual(rows[0]["entity_id"], "aircraft-3")
        self.assertEqual(rows[0]["route_edge_id"], 12)
        self.assertEqual(rows[0]["unreal_location_cm"], {"x": 1250.0, "y": 800.0, "z": 0.0})
        self.assertEqual(rows[0]["unreal_yaw_degrees"], -28.64789)

    def test_missing_pose_is_counted_unknown_and_not_fabricated(self) -> None:
        source = self._events([{"sequence": 0, "time_seconds": 0, "entity_id": "vehicle-1", "entity_type": "ground_vehicle"}])
        rows, counts = unreal_replay_rows(source)
        self.assertEqual(rows, [])
        self.assertEqual(counts["without_pose"], 1)

    def test_related_ground_vehicle_uses_its_own_pose_not_aircraft_pose(self) -> None:
        source = self._events([{"sequence": 1, "time_seconds": 10, "type": "TurnaroundTaskStarted",
            "entity_id": "aircraft-3", "entity_type": "aircraft", "related_entity_id": "vehicle-9", "related_entity_type": "ground_vehicle",
            "related_position_m": {"x": 40, "y": 50}, "related_heading_rad": 1.0}])
        rows, counts = unreal_replay_rows(source)
        self.assertEqual(counts["with_pose"], 1)
        vehicle = next(row for row in rows if row["entity_id"] == "vehicle-9")
        self.assertEqual(vehicle["source_position_m"], {"east": 40.0, "north": 50.0})

    def test_non_monotonic_event_time_fails_closed(self) -> None:
        source = self._events([{"time_seconds": 2}, {"time_seconds": 1}])
        with self.assertRaisesRegex(ValueError, "non-monotonic"):
            unreal_replay_rows(source)

    def test_cross_surface_correlation_requires_aircraft_vehicle_and_disruption_samples(self) -> None:
        events = [
            {"sequence": 1, "time_seconds": 1, "type": "AircraftLanded", "aircraft": "KAUO-G31-GA-01",
             "entity_id": "aircraft-1", "entity_type": "aircraft", "position_m": {"x": 10, "y": 20}, "heading_rad": 0},
            {"sequence": 2, "time_seconds": 241, "type": "TurnaroundDisruptionDetected", "aircraft": "KAUO-G31-GA-03",
             "entity_id": "aircraft-3", "entity_type": "aircraft", "position_m": {"x": 11, "y": 21}, "heading_rad": 0.1},
            {"sequence": 3, "time_seconds": 515, "type": "TurnaroundTaskStarted", "vehicle": "G31-FUEL-01",
             "entity_id": "aircraft-3", "entity_type": "aircraft", "related_entity_id": "vehicle-3",
             "related_entity_type": "ground_vehicle", "related_position_m": {"x": 12, "y": 22}, "related_heading_rad": 0.2},
            {"sequence": 4, "time_seconds": 4319, "type": "SurfaceRunwayClearance", "aircraft": "KAUO-G31-GA-03",
             "entity_id": "aircraft-3", "entity_type": "aircraft", "position_m": {"x": 13, "y": 23}, "heading_rad": 0.3},
        ]
        source = self._events(events)
        adapter_rows, _ = unreal_replay_rows(source)
        adapter = Path(self.temp.name) / "adapter.jsonl"
        adapter.write_text("\n".join(json.dumps(row) for row in adapter_rows) + "\n", encoding="utf-8")
        report = correlate(source, adapter)
        self.assertEqual({row["sample"] for row in report["samples"]},
                         {"arrival", "departure", "pushback_disruption", "ground_service"})
        self.assertTrue(all(row["within_tolerance"] for row in report["samples"]))

    def test_recorded_event_replay_rows_are_byte_deterministic(self) -> None:
        source = self._events([{"sequence": 1, "time_seconds": 2, "type": "SurfaceReservationAcquired",
            "entity_id": "aircraft-2", "entity_type": "aircraft", "position_m": {"x": 1, "y": 2},
            "heading_rad": 0.3}])
        first, second = Path(self.temp.name) / "first.jsonl", Path(self.temp.name) / "second.jsonl"
        write_unreal_replay(source, first)
        write_unreal_replay(source, second)
        self.assertEqual(first.read_bytes(), second.read_bytes())


if __name__ == "__main__":
    unittest.main()
