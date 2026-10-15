"""Deterministic tests for KAUO operational geometry and readiness auditing."""
from __future__ import annotations

import copy
import hashlib
import json
import tempfile
import unittest
from pathlib import Path

from tools.airport_data_ingestion.kauo_geometry import (
    DEFAULT_ANCHORS,
    DEFAULT_DATASET,
    DEFAULT_LEGACY_GEOMETRY,
    DEFAULT_NASR_EXTRACT,
    _dump,
    audit_clearances,
    audit_readiness,
    build_dataset,
    connected_components,
    main,
    point_segment_distance,
    render_svg,
    validate_nasr_extract,
    validate_provenance,
)
from tools.airport_scenario_generation.geospatial import GeographicCoordinate, enu_to_geographic, geographic_to_enu


def _load(path: Path):
    return json.loads(path.read_text(encoding="utf-8"))


class KauoGeometryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.anchors = _load(DEFAULT_ANCHORS)
        cls.legacy = _load(DEFAULT_LEGACY_GEOMETRY)
        cls.extract = _load(DEFAULT_NASR_EXTRACT)
        cls.dataset = build_dataset(cls.anchors, cls.legacy)

    def test_committed_dataset_is_deterministic_and_current(self):
        self.assertEqual(_dump(self.dataset), DEFAULT_DATASET.read_bytes())
        duplicate = build_dataset(copy.deepcopy(self.anchors), copy.deepcopy(self.legacy))
        self.assertEqual(_dump(duplicate), _dump(self.dataset))

    def test_faa_extract_matches_anchor_snapshot(self):
        self.assertEqual([], validate_nasr_extract(self.extract, self.anchors))
        changed = copy.deepcopy(self.extract)
        changed["files"]["APT_RWY_END.csv"]["records"][0]["LAT_DECIMAL"] = "0"
        self.assertTrue(validate_nasr_extract(changed, self.anchors))

    def test_faa_extract_includes_required_records_and_source_archive_hash(self):
        self.assertRegex(self.extract["archive_sha256"], r"^[0-9a-f]{64}$")
        self.assertEqual(1, self.extract["files"]["APT_BASE.csv"]["record_count"])
        self.assertEqual(2, self.extract["files"]["APT_RWY.csv"]["record_count"])
        self.assertEqual(4, self.extract["files"]["APT_RWY_END.csv"]["record_count"])

    def test_all_canonical_and_rejected_geometry_has_complete_provenance(self):
        self.assertEqual([], validate_provenance(self.dataset))
        for feature in self.dataset["features"] + self.dataset["rejected_features"]:
            self.assertIsNone(feature["accuracy_m"])
            self.assertIn(feature["classification"], {"AUTHORITATIVE", "SOURCE_DERIVED", "APPROXIMATE", "SYNTHETIC"})

    def test_missing_provenance_is_rejected(self):
        broken = copy.deepcopy(self.dataset)
        del broken["features"][0]["source_id"]
        errors = validate_provenance(broken)
        self.assertTrue(any("missing provenance field source_id" in error for error in errors))

    def test_approximate_canonical_geometry_cannot_be_simulation_approved(self):
        for classification in ("APPROXIMATE", "SYNTHETIC"):
            broken = copy.deepcopy(self.dataset)
            feature = copy.deepcopy(broken["rejected_features"][0])
            feature["geometry_id"] = f"invalid-promoted-{classification.lower()}"
            feature["classification"] = classification
            feature["simulation_approved"] = True
            feature["authoritative"] = False
            broken["features"].append(feature)
            errors = validate_provenance(broken)
            self.assertTrue(any("cannot be simulation approved" in error for error in errors))
            report = audit_readiness(broken, authoritative_mode=True)
            self.assertEqual("BLOCKED", report["status"])
            self.assertTrue(any(row["code"] == "NON_AUTHORITATIVE_CANONICAL_GEOMETRY" for row in report["findings"]))

    def test_known_faa_runway_and_threshold_counts(self):
        self.assertEqual(2, sum(row["feature_type"] == "runway" for row in self.dataset["features"]))
        self.assertEqual(4, sum(row["feature_type"] == "runway_threshold" for row in self.dataset["features"]))
        self.assertEqual(0, sum(row["feature_type"] == "aircraft_stand" for row in self.dataset["features"]))
        self.assertTrue(all(row["classification"] == "AUTHORITATIVE" for row in self.dataset["features"] if row["feature_type"] == "runway_threshold"))
        self.assertTrue(all(row["classification"] == "SOURCE_DERIVED" for row in self.dataset["features"] if row["feature_type"] == "runway"))

    def test_faa_threshold_coordinate_transform_and_round_trip(self):
        origin_row = self.anchors["origin"]
        origin = GeographicCoordinate(origin_row["latitude_degrees"], origin_row["longitude_degrees"], origin_row["ellipsoid_height_m"])
        threshold = next(row for row in self.dataset["features"] if row.get("threshold_id") == "18")
        expected = self.anchors["runways"][0]["thresholds"]["18"]
        transformed = geographic_to_enu(GeographicCoordinate(expected["latitude_degrees"], expected["longitude_degrees"], origin.ellipsoid_height_m), origin)
        self.assertAlmostEqual(transformed.east_m, threshold["coordinates"][0], places=6)
        self.assertAlmostEqual(transformed.north_m, threshold["coordinates"][1], places=6)
        restored = enu_to_geographic(transformed, origin)
        self.assertLess(abs(restored.latitude_degrees - expected["latitude_degrees"]), 1e-9)
        self.assertLess(abs(restored.longitude_degrees - expected["longitude_degrees"]), 1e-9)

    def test_readiness_fails_closed_with_explicit_missing_data(self):
        report = audit_readiness(self.dataset, authoritative_mode=True)
        self.assertEqual("BLOCKED", report["status"])
        self.assertFalse(report["simulation_authorized"])
        self.assertIn("KAUO DATA READINESS: BLOCKED", report["banner"])
        codes = {row["code"] for row in report["findings"]}
        self.assertTrue({"TAXIWAY_TOPOLOGY_MISSING", "GATE_STAND_CONNECTIVITY_MISSING",
                         "GSE_SPAWN_ACCESSIBILITY_MISSING", "RUNWAY_CROSSING_RULES_MISSING",
                         "AIRCRAFT_ROUTE_CLEARANCE_UNKNOWN", "OPERATIONAL_GRAPH_MISSING"}.issubset(codes))
        self.assertEqual(0, report["feature_counts"]["canonical"]["aircraft_stand"])

    def test_require_ready_cli_returns_blocked_exit_without_running_simulator(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            exit_code = main([
                "--authoritative-mode", "--require-ready",
                "--dataset", str(out / "dataset.json"), "--report", str(out / "report.json"),
                "--svg", str(out / "geometry.svg"),
            ])
            report = json.loads((out / "report.json").read_text(encoding="utf-8"))
            self.assertEqual(2, exit_code)
            self.assertFalse(report["simulation_authorized"])
            self.assertIn("DATA BLOCKED", report["simulation_decision"])

    def test_readiness_reports_are_deterministic(self):
        first = _dump(audit_readiness(self.dataset, authoritative_mode=True))
        second = _dump(audit_readiness(self.dataset, authoritative_mode=True))
        self.assertEqual(first, second)
        self.assertEqual(hashlib.sha256(first).hexdigest(), hashlib.sha256(second).hexdigest())

    def test_complete_source_backed_fixture_can_pass_gate(self):
        candidate = copy.deepcopy(self.dataset)
        fixture_source = "readiness_test_fixture"
        candidate["sources"].append({"source_id": fixture_source, "publisher": "Unit test only"})

        def feature(identity, feature_type, geometry_type="Point"):
            candidate["features"].append({
                "geometry_id": identity, "feature_type": feature_type, "geometry_type": geometry_type,
                "coordinates": [[0.0, 0.0], [1.0, 0.0]] if geometry_type == "LineString" else [0.0, 0.0],
                "coordinate_reference_system": "test fixture",
                "source_id": fixture_source, "source_type": "unit test fixture", "source_date": None,
                "capture_method": "TEST_FIXTURE", "classification": "SOURCE_DERIVED",
                "authoritative": False,
                "accuracy_m": 0.1, "confidence": 1.0, "simulation_approved": True,
                "notes": "Test fixture only; not KAUO source data.",
            })

        feature("taxiway_main", "taxiway_centerline", "LineString")
        feature("taxiway_intersection", "taxiway_intersection")
        feature("stand_1", "aircraft_stand")
        feature("service_area_1", "terminal_service_area")
        feature("gse_spawn_1", "gse_staging_position")
        feature("service_road_1", "service_road", "LineString")
        feature("crossing_1", "runway_crossing")
        graph_nodes = []
        for identity in ("network_a", "network_b"):
            graph_nodes.append({
                "geometry_id": f"graph_{identity}", "node_id": identity,
                "source_id": fixture_source, "source_type": "unit test fixture", "source_date": None,
                "capture_method": "TEST_FIXTURE", "classification": "SOURCE_DERIVED",
                "authoritative": False,
                "accuracy_m": 0.1, "confidence": 1.0, "simulation_approved": True,
                "notes": "Test fixture only; not KAUO source data.",
            })
        candidate["operational_graph"] = {
            "nodes": graph_nodes,
            "edges": [{
                "geometry_id": "graph_edge_ab", "edge_id": "edge_ab", "from": "network_a", "to": "network_b",
                "source_id": fixture_source, "source_type": "unit test fixture", "source_date": None,
                "capture_method": "TEST_FIXTURE", "classification": "SOURCE_DERIVED",
                "authoritative": False,
                "accuracy_m": 0.1, "confidence": 1.0, "simulation_approved": True,
                "notes": "Test fixture only; not KAUO source data.",
            }],
        }
        relations = [
            ("arrival_runway_exit_connectivity", "runway_18_36_threshold_18", "taxiway_main"),
            ("departure_runway_entry_connectivity", "taxiway_main", "runway_18_36_threshold_36"),
            ("gate_stand_connectivity", "stand_1", "taxiway_main"),
            ("turnaround_service_access", "stand_1", "service_area_1"),
            ("gse_spawn_accessibility", "gse_spawn_1", "service_area_1"),
            ("service_road_access", "service_road_1", "service_area_1"),
            ("runway_crossing_permission", "crossing_1", "runway_18_36_centerline"),
        ]
        candidate["operational_relationships"] = [{
            "relation_type": kind, "from_feature_id": start, "to_feature_id": end,
            "source_id": fixture_source, "simulation_approved": True, "inferred": False,
        } for kind, start, end in relations]
        candidate["clearance_assessments"] = [
            {"assessment_id": "aircraft", "assessment_type": "aircraft_route", "required_clearance_m": 2.0, "measured_clearance_m": 2.1,
             "source_id": fixture_source, "assessment_method": "test fixture", "simulation_approved": True, "confidence": 1.0},
            {"assessment_id": "gse", "assessment_type": "stationary_gse", "required_clearance_m": 1.0, "measured_clearance_m": 1.2,
             "source_id": fixture_source, "assessment_method": "test fixture", "simulation_approved": True, "confidence": 1.0},
        ]
        report = audit_readiness(candidate, authoritative_mode=True)
        self.assertEqual("READY", report["status"], report["findings"])
        self.assertTrue(report["simulation_authorized"])

    def test_components_detect_disconnection_and_missing_edges(self):
        self.assertEqual([["a", "b"], ["z"]], connected_components(
            ["b", "a", "z"], [{"id": "ab", "from": "a", "to": "b"}]
        ))
        with self.assertRaisesRegex(ValueError, "missing graph node"):
            connected_components(["a"], [{"id": "bad", "from": "a", "to": "x"}])

    def test_clearance_audit_preserves_unknown_and_detects_failure(self):
        rows = audit_clearances([
            {"assessment_id": "pass", "required_clearance_m": 2.0, "measured_clearance_m": 2.5,
             "source_id": "source", "assessment_method": "fixture", "simulation_approved": True, "confidence": 1.0},
            {"assessment_id": "fail", "required_clearance_m": 2.0, "measured_clearance_m": 1.9,
             "source_id": "source", "assessment_method": "fixture", "simulation_approved": True, "confidence": 1.0},
            {"assessment_id": "unknown", "required_clearance_m": 2.0, "measured_clearance_m": None,
             "source_id": "source", "assessment_method": "fixture", "simulation_approved": True, "confidence": 1.0},
        ])
        self.assertEqual(["FAIL", "PASS", "UNKNOWN"], [row["status"] for row in rows])

    def test_point_to_segment_clearance_distance(self):
        self.assertAlmostEqual(3.0, point_segment_distance((2.0, 3.0), (0.0, 0.0), (4.0, 0.0)))
        self.assertAlmostEqual(5.0, point_segment_distance((3.0, 4.0), (0.0, 0.0), (0.0, 0.0)))

    def test_visualization_explains_source_derived_and_quarantined_data(self):
        svg = render_svg(self.dataset)
        self.assertIn("FAA threshold endpoints", svg)
        self.assertIn("Legacy approximate nodes/edges, rejected", svg)
        self.assertIn("Not a georeferenced FAA chart overlay", svg)
        self.assertEqual(svg, render_svg(self.dataset))

    def test_dataset_contains_no_synthetic_operational_features(self):
        self.assertFalse(any(row["classification"] == "SYNTHETIC" for row in self.dataset["features"]))
        self.assertFalse(any(row["simulation_approved"] for row in self.dataset["rejected_features"]))


if __name__ == "__main__":
    unittest.main()
