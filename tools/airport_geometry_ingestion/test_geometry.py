"""Goal46 schema, importer, graph, readiness, comparison, and reproducibility tests."""
from __future__ import annotations

import copy
import contextlib
import hashlib
import io
import json
import tempfile
import unittest
import zipfile
from pathlib import Path

from .core import (GeometryInputError, audit_clearances, build_graph, canonical_bytes,
                   compare_datasets, convert_goal45_dataset, import_source, readiness,
                   validate_dataset)
from .fixtures import make_fixture, write_fixtures
from .cli import main as cli_main

ROOT=Path(__file__).resolve().parents[2]
GOAL45=ROOT/"tools/airport_data_ingestion/kauo_operational_geometry.json"
ORIGIN={"latitude_degrees":32.61511111,"longitude_degrees":-85.434,"ellipsoid_height_m":208.22}


def write_mapping(path: Path, *, crs="EPSG:4326", units="degrees", fields=None, source_id="mapping_test"):
    mapping={"dataset_id":"mapping_test","airport":{"airport_id":"TEST"},"source":{"source_id":source_id,
              "source_type":"test source","authority":"SYNTHETIC","extraction_method":"unit test mapping"},
             "source_crs":crs,"source_units":units,"origin":ORIGIN,
             "field_mapping":fields or {"id":"fid","feature_type":"kind"}}
    path.write_bytes(canonical_bytes(mapping))


class Goal46GeometryTests(unittest.TestCase):
    def test_goal45_import_retains_seven_features_and_quarantines_148(self):
        dataset=convert_goal45_dataset(GOAL45)
        self.assertEqual("2.1.0",dataset["schema_version"])
        self.assertTrue(any(s["source_id"] == "faa_d_tpp_airport_diagram_2610" for s in dataset["sources"]))
        self.assertEqual(7,len(dataset["features"]))
        self.assertEqual(148,len(dataset["quarantined_features"]))
        self.assertEqual(0,len(validate_dataset(dataset)))
        report=readiness(dataset)
        self.assertEqual("DATA_BLOCKED",report["decision"])
        self.assertFalse(report["production_simulator_allowed"])
        ids=[f["geometry_id"] for f in dataset["features"]]
        self.assertEqual(1,sum(f["feature_type"]=="airport_reference_point" for f in dataset["features"]))
        self.assertEqual(4,sum(f["feature_type"]=="runway_threshold" for f in dataset["features"]))
        self.assertEqual(2,sum(f["feature_type"]=="runway" for f in dataset["features"]))
        self.assertTrue(all(f["provenance"]["authority"]!="APPROXIMATE" for f in dataset["features"]))
        self.assertTrue(all(f["provenance"]["authority"]=="APPROXIMATE" and not f["provenance"]["simulation_approved"] for f in dataset["quarantined_features"]))
        self.assertTrue(ids)
        missing={r["requirement"] for r in report["requirements"] if r["status"] in {"BLOCKED","UNKNOWN","FAIL"}}
        self.assertTrue({"TAXIWAY_GEOMETRY","TAXIWAY_CONNECTIVITY","STAND_ACCESS","GSE_ACCESS","RUNWAY_CROSSINGS","ROUTE_CLEARANCES"}.issubset(missing))

    def test_geojson_wgs84_transform_is_axis_explicit_and_reproducible(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder=Path(tmp); source=folder/"features.geojson"; mapping=folder/"mapping.json"
            source.write_text(json.dumps({"type":"FeatureCollection","features":[
                {"type":"Feature","id":"p1","properties":{"fid":"p1","kind":"taxiway_intersection"},
                 "geometry":{"type":"Point","coordinates":[-85.433,32.61511111]}}]}),encoding="utf-8")
            write_mapping(mapping)
            first=import_source(source,mapping); second=import_source(source,mapping)
            self.assertEqual(canonical_bytes(first),canonical_bytes(second))
            point=first["features"][0]["coordinates"]
            self.assertGreater(point[0],90.0)
            self.assertLess(abs(point[1]),0.01)
            self.assertEqual([],validate_dataset(first))
            self.assertIn("EPSG:4326 to local ENU",first["features"][0]["provenance"]["transformation"])

    def test_csv_point_edge_mapping_and_no_field_guessing(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder=Path(tmp); source=folder/"edges.csv"; mapping=folder/"mapping.json"
            source.write_text("fid,kind,sx,sy,ex,ey\nedge_a,taxiway_centerline,1,2,5,2\n",encoding="utf-8")
            write_mapping(mapping,crs="LOCAL_ENU",units="m",fields={"id":"fid","feature_type":"kind",
                         "coordinate_fields":{"edge":{"start_x":"sx","start_y":"sy","end_x":"ex","end_y":"ey"}}})
            dataset=import_source(source,mapping)
            self.assertEqual([[1.0,2.0],[5.0,2.0]],dataset["features"][0]["coordinates"])
            self.assertEqual([],validate_dataset(dataset))
            bad=folder/"bad.json"; write_mapping(bad,fields={"feature_type":"kind"})
            with self.assertRaisesRegex(GeometryInputError,"must explicitly name id"):
                import_source(source,bad)

    def test_kml_and_kmz_are_supported_with_explicit_extended_data_mapping(self):
        kml=b'''<kml xmlns="http://www.opengis.net/kml/2.2"><Document><Placemark><name>crossing</name><ExtendedData><Data name="kind"><value>runway_crossing</value></Data></ExtendedData><LineString><coordinates>-85.434,32.61511111,0 -85.4339,32.61511111,0</coordinates></LineString></Placemark></Document></kml>'''
        with tempfile.TemporaryDirectory() as tmp:
            folder=Path(tmp); mapping=folder/"mapping.json"; write_mapping(mapping,fields={"id":"name","feature_type":"kind"})
            source=folder/"crossing.kml"; source.write_bytes(kml)
            single=import_source(source,mapping)
            self.assertEqual("runway_crossing",single["features"][0]["feature_type"])
            kmz=folder/"crossing.kmz"
            with zipfile.ZipFile(kmz,"w") as archive: archive.writestr("doc.kml",kml)
            zipped=import_source(kmz,mapping)
            self.assertEqual(single["features"][0]["coordinates"],zipped["features"][0]["coordinates"])
            self.assertNotEqual(single["sources"][0]["source_sha256"],zipped["sources"][0]["source_sha256"])
            self.assertEqual([0.0,0.0],single["features"][0]["properties"]["source_kml_altitude_m"])
            self.assertEqual(2,len(single["features"][0]["coordinates"]))

    def test_local_simulator_transform_requires_bearing_and_explicit_units(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder=Path(tmp); source=folder/"point.csv"; mapping=folder/"mapping.json"
            source.write_text("fid,kind,x_ft,y_ft\np1,taxiway_intersection,10,0\n",encoding="utf-8")
            fields={"id":"fid","feature_type":"kind","coordinate_fields":{"point":{"x":"x_ft","y":"y_ft"}}}
            write_mapping(mapping,crs="LOCAL_SIMULATOR",units="ft",fields=fields)
            config=json.loads(mapping.read_text(encoding="utf-8")); config["local_transform"]={"positive_x_true_bearing_degrees":0}
            mapping.write_bytes(canonical_bytes(config))
            dataset=import_source(source,mapping)
            point=dataset["features"][0]["coordinates"]
            self.assertAlmostEqual(0.0,point[0],places=6)
            self.assertAlmostEqual(3.048,point[1],places=6)
            config.pop("local_transform"); mapping.write_bytes(canonical_bytes(config))
            with self.assertRaisesRegex(GeometryInputError,"positive_x_true_bearing_degrees"):
                import_source(source,mapping)

    def test_cli_validation_blocked_exit_and_deterministic_evidence_bundle(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder=Path(tmp); dataset_path=folder/"fixture_A.json"; dataset_path.write_bytes(canonical_bytes(make_fixture("A")))
            report=folder/"validate.json"
            self.assertEqual(0,cli_main(["validate",str(dataset_path),"--report",str(report)]))
            blocked=folder/"blocked.json"
            self.assertEqual(2,cli_main(["readiness",str(dataset_path),"--json",str(blocked),"--profile","production","--require-ready"]))
            self.assertEqual("DATA_BLOCKED",json.loads(blocked.read_text(encoding="utf-8"))["decision"])
            evidence=folder/"evidence"; evidence.mkdir(); (evidence/"artifact.txt").write_text("evidence\n",encoding="utf-8")
            archive=folder/"bundle.zip"
            self.assertEqual(0,cli_main(["bundle",str(evidence),"--output",str(archive)]))
            first=archive.read_bytes()
            self.assertEqual(0,cli_main(["bundle",str(evidence),"--output",str(archive)]))
            self.assertEqual(first,archive.read_bytes())
            with zipfile.ZipFile(archive) as zipped: self.assertIsNone(zipped.testzip())

    def test_shapefile_and_projected_crs_fail_as_actionable_unsupported_inputs(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder=Path(tmp); shp=folder/"roads.shp"; shp.write_bytes(b"unsupported test input")
            mapping=folder/"mapping.json"; write_mapping(mapping,crs="EPSG:26916")
            with self.assertRaisesRegex(GeometryInputError,"unsupported source format"):
                import_source(shp,mapping)
            source=folder/"projected.geojson"
            source.write_text(json.dumps({"type":"FeatureCollection","features":[{"type":"Feature","properties":{"fid":"p","kind":"taxiway_intersection"},"geometry":{"type":"Point","coordinates":[500000,3600000]}}]}),encoding="utf-8")
            with self.assertRaisesRegex(GeometryInputError,"unsupported source CRS"):
                import_source(source,mapping)

    def test_schema_requires_complete_provenance_and_disallows_approximate_approval(self):
        dataset=make_fixture("A")
        bad=copy.deepcopy(dataset)
        del bad["features"][0]["provenance"]["source_id"]
        self.assertTrue(any(f["code"]=="PROVENANCE_FIELD" for f in validate_dataset(bad)))
        bad=copy.deepcopy(dataset)
        bad["features"][0]["provenance"]["authority"]="APPROXIMATE"
        bad["features"][0]["provenance"]["simulation_approved"]=True
        self.assertTrue(any(f["code"]=="FORBIDDEN_SIMULATION_APPROVAL" for f in validate_dataset(bad)))

    def test_graph_snapping_tolerance_lineage_and_no_gap_bridging(self):
        dataset=make_fixture("A")
        beta=copy.deepcopy(dataset["features"][4])
        beta["geometry_id"]="taxiway_beta"
        beta["provenance"]["original_feature_id"]="taxiway_beta"
        beta["coordinates"]=[[20.1,10],[50,10]]
        dataset["features"].append(beta)
        build_graph(dataset,0.15)
        shared=[n for n in dataset["operational_graph"]["nodes"] if {"taxiway_alpha","taxiway_beta"}.issubset(n["source_feature_ids"])]
        self.assertEqual(1,len(shared))
        self.assertTrue({"taxiway_alpha","taxiway_beta"}.issubset(shared[0]["provenance"]["derived_from"]))
        gap=make_fixture("A")
        beta["coordinates"]=[[20.3,10],[50,10]]
        gap["features"].append(beta)
        build_graph(gap,0.15)
        from tools.airport_geometry_ingestion.core import graph_diagnostics
        self.assertTrue(any(x["code"]=="DISCONNECTED_COMPONENTS" for x in graph_diagnostics(gap)))

    def test_intersections_near_misses_self_crossings_and_zero_length_segments_are_diagnosed(self):
        from tools.airport_geometry_ingestion.core import graph_diagnostics
        dataset=make_fixture("A")
        crossing=copy.deepcopy(dataset["features"][4]); crossing["geometry_id"]="taxiway_crossing"
        crossing["provenance"]["original_feature_id"]="taxiway_crossing"
        crossing["coordinates"]=[[10,0],[10,20]]
        dataset["features"].append(crossing); build_graph(dataset,0.01)
        self.assertTrue(any(r["code"]=="AMBIGUOUS_INTERSECTION" for r in graph_diagnostics(dataset)))
        near=make_fixture("A")
        miss=copy.deepcopy(near["features"][4]); miss["geometry_id"]="taxiway_near_miss"
        miss["provenance"]["original_feature_id"]="taxiway_near_miss"
        miss["coordinates"]=[[20.5,10],[50,10]]
        near["features"].append(miss); build_graph(near,0.25)
        self.assertTrue(any(r["code"]=="SUSPICIOUS_NEAR_MISS" for r in graph_diagnostics(near)))
        looping=make_fixture("A")
        loop=copy.deepcopy(looping["features"][4]); loop["geometry_id"]="taxiway_loop"
        loop["provenance"]["original_feature_id"]="taxiway_loop"
        loop["coordinates"]=[[50,50],[51,51],[50,51],[51,50]]
        loop["coordinates"].insert(1,[50,50])
        looping["features"].append(loop); build_graph(looping,0.01)
        codes={row["code"] for row in graph_diagnostics(looping)}
        self.assertIn("SELF_INTERSECTION",codes)
        self.assertIn("ZERO_LENGTH_SEGMENT",codes)

    def test_directionality_and_vehicle_permissions_are_never_assumed(self):
        dataset=make_fixture("A")
        dataset["features"][4]["properties"]["directed"]=True
        dataset["features"][4]["properties"].pop("permissions")
        build_graph(dataset,0.01)
        report=readiness(dataset,profile="test_fixture")
        self.assertEqual("BLOCKED",next(r["status"] for r in report["requirements"] if r["requirement"]=="STAND_ACCESS"))
        codes={row["code"] for row in report["graph_diagnostics"]}
        self.assertIn("EDGE_PERMISSIONS_UNKNOWN",codes)

    def test_eight_synthetic_fixture_readiness_outcomes(self):
        expected={"A":"SIMULATOR_READY","B":"DATA_BLOCKED","C":"DATA_BLOCKED","D":"DATA_BLOCKED",
                  "E":"DATA_BLOCKED","F":"DATA_BLOCKED","G":"DATA_BLOCKED","H":"DATA_BLOCKED"}
        actual={letter:readiness(make_fixture(letter),profile="test_fixture") for letter in expected}
        self.assertEqual(expected,{letter:row["decision"] for letter,row in actual.items()})
        self.assertFalse(actual["A"]["production_simulator_allowed"])
        self.assertTrue(any(r["status"]=="FAIL" for r in actual["B"]["topology_routes"]["stand_to_runway"]))
        self.assertTrue(any(r["status"]=="FAIL" for r in actual["C"]["topology_routes"]["gse_to_stand"]))
        self.assertEqual("BLOCKED",next(r["status"] for r in actual["D"]["requirements"] if r["requirement"]=="RUNWAY_CROSSINGS"))
        self.assertEqual("UNKNOWN",next(r["status"] for r in actual["E"]["requirements"] if r["requirement"]=="ROUTE_CLEARANCES"))
        self.assertEqual("FAIL",next(r["status"] for r in actual["F"]["requirements"] if r["requirement"]=="PROVENANCE_COMPLETENESS"))
        self.assertEqual("BLOCKED",next(r["status"] for r in readiness(make_fixture("G"),profile="production")["requirements"] if r["requirement"]=="SOURCE_AUTHORITY"))
        self.assertEqual("BLOCKED",next(r["status"] for r in actual["H"]["requirements"] if r["requirement"]=="STAND_ACCESS"))

    def test_clearance_status_is_pass_fail_or_unknown_and_authority_gated(self):
        rows=[{"assessment_id":"pass","required_clearance_m":2.0,"measured_clearance_m":2.2,"source_id":"s","assessment_method":"survey","simulation_approved":True},
              {"assessment_id":"fail","required_clearance_m":2.0,"measured_clearance_m":1.9,"source_id":"s","assessment_method":"survey","simulation_approved":True},
              {"assessment_id":"unknown","required_clearance_m":2.0,"measured_clearance_m":2.2,"source_id":"s","assessment_method":"survey","simulation_approved":False}]
        self.assertEqual(["FAIL","PASS","UNKNOWN"],[r["status"] for r in audit_clearances(rows)])

    def test_dataset_comparison_reports_added_moved_permissions_and_readiness(self):
        before,after=make_fixture("A"),make_fixture("H")
        report=compare_datasets(before,after,profile="test_fixture")
        self.assertEqual("SIMULATOR_READY",report["readiness_change"]["before"])
        self.assertEqual("DATA_BLOCKED",report["readiness_change"]["after"])
        taxi=next(row for row in report["changed_features"] if row["geometry_id"]=="taxiway_alpha")
        changes={c["change"] for c in taxi["changes"]}
        self.assertIn("moved",changes)
        self.assertIn("properties_permissions_clearances",changes)
        extra=copy.deepcopy(after)
        extra["features"].append(copy.deepcopy(extra["features"][0]))
        extra["features"][-1]["geometry_id"]="added_feature"
        extra["features"][-1]["provenance"]["original_feature_id"]="added_feature"
        diff=compare_datasets(before,extra,profile="test_fixture")
        self.assertIn("added_feature",diff["added_features"])

    def test_fixture_generation_and_canonical_serialization_are_byte_deterministic(self):
        one,two=make_fixture("A"),make_fixture("A")
        self.assertEqual(canonical_bytes(one),canonical_bytes(two))
        self.assertEqual(hashlib.sha256(canonical_bytes(one)).hexdigest(),hashlib.sha256(canonical_bytes(two)).hexdigest())
        with tempfile.TemporaryDirectory() as tmp:
            a=write_fixtures(Path(tmp)/"one"); b=write_fixtures(Path(tmp)/"two")
            self.assertEqual([canonical_bytes(a[k]) for k in "ABCDEFGH"],[canonical_bytes(b[k]) for k in "ABCDEFGH"])

    def test_source_derived_confidence_and_routing_eligibility_are_fail_closed(self):
        dataset=make_fixture("A")
        feature=dataset["features"][4]
        source_id="source_derived_test"
        source_hash=hashlib.sha256(b"synthetic test evidence only").hexdigest()
        dataset["sources"].extend([
            {"source_id":source_id,"source_type":"synthetic test evidence","source_file":"test-evidence.txt",
             "source_sha256":source_hash,"publication_date":None,"extraction_method":"test only","authority":"SOURCE_DERIVED",
             "notes":"Fixture for confidence-policy tests only; not real airport evidence."},
            {"source_id":"independent_test_source","source_type":"synthetic test corroboration","source_file":"corroboration.txt",
             "source_sha256":source_hash,"publication_date":None,"extraction_method":"test only","authority":"AUTHORITATIVE",
             "notes":"Fixture for confidence-policy tests only; not real airport evidence."},
        ])
        provenance=feature["provenance"]
        provenance.update({"authority":"SOURCE_DERIVED","source_id":source_id,"source_file":"test-evidence.txt",
                           "source_sha256":source_hash,"confidence_level":"HIGH","accuracy_m":0.5,
                           "geometry_origin":"derived","derived_from":["apron_alpha"],"simulation_approved":True,
                           "derivation":{"configuration_sha256":hashlib.sha256(b"deterministic test derivation").hexdigest(),
                             "corroborating_source_ids":["independent_test_source"],
                             "validation_checks":{"crs":True,"airport_bounds":True,"geometry":True,"independent_review":True}}})
        self.assertFalse(any(row["code"].startswith("SOURCE_DERIVED") for row in validate_dataset(dataset)))
        self.assertEqual("SOURCE_DERIVED", provenance["authority"])
        high_readiness=readiness(dataset,profile="production")
        self.assertEqual("PASS",next(r["status"] for r in high_readiness["requirements"] if r["requirement"]=="TAXIWAY_GEOMETRY"))
        provenance["confidence_level"]="MEDIUM"
        self.assertTrue(any(row["code"]=="SOURCE_DERIVED_APPROVAL" for row in validate_dataset(dataset)))
        provenance["simulation_approved"]=False
        low_readiness=readiness(dataset,profile="production")
        self.assertEqual("BLOCKED",next(r["status"] for r in low_readiness["requirements"] if r["requirement"]=="TAXIWAY_GEOMETRY"))
        provenance["confidence_level"]="HIGH"
        provenance["simulation_approved"]=True
        provenance["accuracy_m"]=None
        self.assertTrue(any(row["code"]=="SOURCE_DERIVED_HIGH" for row in validate_dataset(dataset)))


if __name__=="__main__": unittest.main()
