"""Focused Goal47 source, provenance, and deterministic dataset checks."""
from __future__ import annotations

import hashlib
import unittest

from .core import canonical_bytes, validate_dataset
from .goal47 import build_dataset, summarize


class Goal47DatasetTests(unittest.TestCase):
    def test_public_geometry_is_preserved_without_promoting_unverified_routes(self):
        dataset = build_dataset()
        summary = summarize(dataset)
        self.assertEqual(99, len(dataset["features"]))
        self.assertEqual(151, len(dataset["quarantined_features"]))
        self.assertEqual(69, len(dataset["operational_graph"]["nodes"]))
        self.assertEqual(55, len(dataset["operational_graph"]["edges"]))
        self.assertEqual({"nodes": 0, "edges": 0}, summary["operational_graph"])
        self.assertEqual({"nodes": 69, "edges": 55}, summary["candidate_graph"])
        self.assertEqual(0, summary["routing_eligible_active_feature_count"])
        self.assertEqual(69, summary["network"]["candidate_taxiway_nodes"])
        self.assertEqual(55, summary["network"]["candidate_taxiway_edges"])
        self.assertEqual(17, summary["network"]["candidate_connected_components"])
        self.assertEqual(0, len(validate_dataset(dataset)))

        active = dataset["features"]
        quarantined = dataset["quarantined_features"]
        by_type = {kind: sum(f["feature_type"] == kind for f in active) for kind in {f["feature_type"] for f in active}}
        self.assertEqual(23, by_type["taxiway_polygon"])
        self.assertEqual(6, by_type["runway_pavement_polygon"])
        self.assertEqual(2, by_type["apron"])
        self.assertEqual(55, by_type["taxiway_centerline"])
        self.assertEqual(3, sum(f["feature_type"] == "taxiway_centerline" for f in quarantined))
        self.assertTrue(all(f["provenance"]["authority"] == "APPROXIMATE" for f in quarantined))

        candidate_lines = [f for f in active if f["feature_type"] == "taxiway_centerline"]
        self.assertTrue(all(f["provenance"]["authority"] == "SOURCE_DERIVED" for f in candidate_lines))
        self.assertTrue(all(f["provenance"]["confidence_level"] == "MEDIUM" for f in candidate_lines))
        self.assertTrue(all(f["provenance"]["accuracy_m"] is None for f in candidate_lines))
        self.assertTrue(all(not f["provenance"]["simulation_approved"] for f in candidate_lines))
        self.assertTrue(all(not f["properties"]["permissions"] for f in candidate_lines))

    def test_build_and_summary_are_byte_deterministic(self):
        first, second = build_dataset(), build_dataset()
        self.assertEqual(canonical_bytes(first), canonical_bytes(second))
        self.assertEqual(hashlib.sha256(canonical_bytes(first)).hexdigest(), summarize(second)["dataset_sha256"])
        counts = summarize(first)["feature_counts_by_authority_and_confidence"]["SOURCE_DERIVED"]
        self.assertEqual(57, counts["MEDIUM"])
        self.assertEqual(0, counts.get("HIGH", 0))


if __name__ == "__main__":
    unittest.main()
