"""Goal 42 ALDOT geometry normalization and provenance regression tests."""
from __future__ import annotations

import copy
import json
from pathlib import Path
import tempfile
import unittest

from tools.airport_scenario_generation.generator import load_mapping
from tools.airport_scenario_generation.goal42_geometry import (
    OUTPUT,
    SOURCE,
    build_geometry,
    graph_alignment_audit,
    normalize_source,
)


class Goal42GeometryTests(unittest.TestCase):
    def test_normalizes_all_classified_aldot_auo_inventory_features(self):
        result = normalize_source()
        areas = result["surface_areas"]
        self.assertEqual(len(areas), 37)
        self.assertEqual({key: sum(area["use"] == key for area in areas) for key in
                          ("RUNWAY", "TAXIWAY", "APRON", "UNCLASSIFIED_OTHER")},
                         {"RUNWAY": 6, "TAXIWAY": 23, "APRON": 2, "UNCLASSIFIED_OTHER": 6})
        self.assertEqual(len({area["id"] for area in areas}), len(areas))
        self.assertTrue(all(area["polygons"] for area in areas))
        self.assertTrue(all(point["x_m"] > -1000 and point["x_m"] < 1200 and
                            point["y_m"] > -1300 and point["y_m"] < 1000
                            for area in areas for poly in area["polygons"]
                            for ring in poly for point in ring))

    def test_goal42_geometry_is_reproducible_and_loadable_by_existing_generator(self):
        geometry = build_geometry()
        encoded = json.dumps(geometry, indent=2, sort_keys=True) + "\n"
        self.assertEqual(OUTPUT.read_text(encoding="utf-8"), encoded)
        self.assertEqual(geometry["source_snapshot_sha256"], normalize_source()["source_sha256"])
        self.assertEqual(geometry["airport"]["geometry_provenance"]["stands_and_gates"]["classification"], "APPROXIMATE")
        self.assertIn("Alabama Department of Transportation", geometry["airport"]["geometry_provenance"]["runway_taxiway_apron_pavement_areas"]["source"])
        mapping, _, loaded, _ = load_mapping(OUTPUT.parent / "mapping.kauo_goal42.json")
        self.assertEqual(mapping["geometry_file"], OUTPUT.name)
        self.assertEqual(len(loaded["airport"]["surface_areas"]), 37)

    def test_normalized_polygon_matches_deterministic_geospatial_transform(self):
        source = json.loads(SOURCE.read_text(encoding="utf-8-sig"))
        feature = next(item for item in source["features"] if item["properties"].get("BRANCHID") == "A01")
        area = next(item for item in normalize_source()["surface_areas"] if item["source_feature_id"] == str(feature["properties"]["OBJECTID"]))
        lon, lat = feature["geometry"]["coordinates"][0][0]
        from tools.airport_scenario_generation.geospatial import GeographicCoordinate, geographic_to_enu
        origin = GeographicCoordinate(32.61511111, -85.434, 208.22)
        local = geographic_to_enu(GeographicCoordinate(lat, lon, origin.ellipsoid_height_m), origin)
        self.assertAlmostEqual(area["polygons"][0][0][0]["x_m"], local.east_m, places=3)
        self.assertAlmostEqual(area["polygons"][0][0][0]["y_m"], local.north_m, places=3)

    def test_missing_geometry_and_open_rings_fail_closed(self):
        source = json.loads(SOURCE.read_text(encoding="utf-8-sig"))
        for mutate in (
            lambda doc: doc["features"][0].update(geometry=None),
            lambda doc: doc["features"][0]["geometry"]["coordinates"][0].pop(),
        ):
            invalid = copy.deepcopy(source)
            mutate(invalid)
            with tempfile.TemporaryDirectory() as temp:
                path = Path(temp) / "bad.geojson"
                path.write_text(json.dumps(invalid), encoding="utf-8")
                with self.assertRaises(ValueError):
                    normalize_source(path)

    def test_scenario_studio_bootstrap_exposes_goal42_surface_provenance(self):
        from tools.ops_dashboard.scenario_studio import bootstrap
        payload = bootstrap("kauo-goal42")
        self.assertEqual(payload["airport"]["id"], "KAUO")
        self.assertEqual(len(payload["geometry"]["surface_areas"]), 37)
        self.assertIn("ALDOT pavement inventory", payload["catalog"]["available_bases"][-2]["provenance"])
        self.assertEqual(payload["geometry"]["geometry_provenance"]["route_graph"]["classification"],
                         "SOURCE_BACKED_DERIVED_WITH_MANUALLY_RECONCILED_APPROXIMATIONS")

    def test_duplicate_source_ids_are_rejected(self):
        source = json.loads(SOURCE.read_text(encoding="utf-8-sig"))
        source["features"][1]["properties"]["OBJECTID"] = source["features"][0]["properties"]["OBJECTID"]
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "duplicate.geojson"
            path.write_text(json.dumps(source), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "duplicate"):
                normalize_source(path)

    def test_goal42_routes_are_derived_inside_pavement_and_classed_by_movement(self):
        airport = build_geometry()["airport"]
        audit = graph_alignment_audit(airport, airport["surface_areas"])
        self.assertEqual(audit["edge_count"], len(airport["edges"]))
        self.assertEqual({row["surface_polygon_coverage_ratio"] for row in audit["edges"]}, {1.0})
        model = airport["surface_routing_model"]
        self.assertEqual(model["route_counts"], {"aircraft": 94, "ground_vehicle": 89, "unavailable_for_both": 0})
        self.assertTrue(all(edge["surface_centerline"] and edge["surface_route_distance_m"] > 0
                            for edge in airport["edges"]))
        self.assertTrue(all(edge["aircraft_allowed"] for edge in airport["edges"]))
        self.assertTrue(any(not edge["ground_vehicle_allowed"] for edge in airport["edges"]))
        self.assertEqual({edge["id"] for edge in airport["edges"] if edge["surface_route_crosses_runway"]},
                         {"runway36_hold_twy_a_south", "twy_a_mid_fbo_primary", "twy_a_north_fbo_detour",
                          "twy_c_fbo_detour", "fbo_junction_fuel_farm"})
        self.assertTrue(all(not edge["ground_vehicle_allowed"] for edge in airport["edges"]
                            if edge["surface_route_crosses_runway"]))
        self.assertTrue(all(node["surface_attachment"]["source_feature_id"]
                            for node in airport["nodes"]))


if __name__ == "__main__":
    unittest.main()
