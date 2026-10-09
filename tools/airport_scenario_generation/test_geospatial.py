"""Focused tests for reusable airport geospatial conversions."""
import math
import unittest

from tools.airport_scenario_generation.geospatial import (
    GeographicCoordinate,
    LocalEnu,
    enu_to_geographic,
    enu_to_unreal_centimeters,
    geographic_to_enu,
    rotate_local_grid_to_enu,
    true_bearing_unit_vector,
)


class AirportGeospatialTests(unittest.TestCase):
    def setUp(self):
        self.origin = GeographicCoordinate(32.61511111, -85.434, 208.22)

    def test_origin_converts_to_zero(self):
        self.assertEqual(geographic_to_enu(self.origin, self.origin), LocalEnu(0.0, 0.0, 0.0))

    def test_known_cardinal_offsets_and_unreal_axes(self):
        for point, expected in (
            (LocalEnu(250.0, 0.0), (250.0, 0.0)),
            (LocalEnu(-250.0, 0.0), (-250.0, 0.0)),
            (LocalEnu(0.0, 250.0), (0.0, 250.0)),
            (LocalEnu(0.0, -250.0), (0.0, -250.0)),
        ):
            round_trip = geographic_to_enu(enu_to_geographic(point, self.origin), self.origin)
            self.assertAlmostEqual(round_trip.east_m, expected[0], places=6)
            self.assertAlmostEqual(round_trip.north_m, expected[1], places=6)
        self.assertEqual(enu_to_unreal_centimeters(LocalEnu(12.5, 7.0, 2.0)), (1250.0, -700.0, 200.0))

    def test_true_heading_convention(self):
        for bearing, expected in ((0, (0, 1)), (90, (1, 0)), (180, (0, -1)), (270, (-1, 0))):
            east, north = true_bearing_unit_vector(bearing)
            self.assertAlmostEqual(east, expected[0], places=12)
            self.assertAlmostEqual(north, expected[1], places=12)

    def test_declared_legacy_heading_rotation(self):
        self.assertAlmostEqual(rotate_local_grid_to_enu(
            100.0, 0.0, positive_x_true_bearing_degrees=90.0
        ).east_m, 100.0, places=12)
        self.assertAlmostEqual(rotate_local_grid_to_enu(
            0.0, 100.0, positive_x_true_bearing_degrees=90.0
        ).north_m, 100.0, places=12)

    def test_distance_and_round_trip_are_preserved(self):
        point = LocalEnu(500.0, -1200.0, 35.0)
        geodetic = enu_to_geographic(point, self.origin)
        round_trip = geographic_to_enu(geodetic, self.origin)
        self.assertAlmostEqual(math.hypot(round_trip.east_m, round_trip.north_m), math.hypot(500.0, 1200.0), places=6)
        self.assertAlmostEqual(round_trip.east_m, point.east_m, places=6)
        self.assertAlmostEqual(round_trip.north_m, point.north_m, places=6)
        self.assertAlmostEqual(round_trip.up_m, point.up_m, places=5)

    def test_non_finite_and_out_of_range_coordinates_fail(self):
        with self.assertRaises(ValueError):
            geographic_to_enu(GeographicCoordinate(91.0, 0.0), self.origin)
        with self.assertRaises(ValueError):
            true_bearing_unit_vector(math.nan)


if __name__ == "__main__":
    unittest.main()
