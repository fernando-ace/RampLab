"""Unmistakably synthetic readiness and regression fixtures (never airport data)."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path

from .core import authority_policy, build_graph, canonical_bytes
from . import SCHEMA_VERSION


def _feature(identity, kind, geom, coords, source_id, source_hash, properties=None, authority="SYNTHETIC", approved=False):
    return {"geometry_id":identity,"feature_type":kind,"geometry_type":geom,"coordinates":coords,
            "properties":properties or {},
            "provenance":{"source_id":source_id,"original_feature_id":identity,"source_file":"synthetic-fixture-source.json",
                          "source_sha256":source_hash,"source_url":None,"publication_date":"synthetic-fixture-v1",
                          "extraction_method":"deterministic synthetic test fixture generator",
                          "transformation":"Fixture LOCAL_ENU metres retained; no geodetic claim.",
                          "authority":authority,"geometry_origin":"direct","accuracy_m":0.0,
                          "simulation_approved":approved}}


def make_fixture(letter: str) -> dict:
    letter=letter.upper()
    if letter not in "ABCDEFGH": raise ValueError("fixture id must be A through H")
    source_id=f"synthetic_goal46_fixture_{letter}"
    source_bytes=canonical_bytes({"fixture":letter,"label":"SYNTHETIC TEST DATA ONLY","version":1})
    source_hash=hashlib.sha256(source_bytes).hexdigest()
    features=[
        _feature("airport_ref","airport_reference_point","Point",[0,0],source_id,source_hash),
        _feature("runway_09_27","runway","LineString",[[0,0],[0,100]],source_id,source_hash,{"runway_id":"09/27","width_m":18.0}),
        _feature("threshold_09","runway_threshold","Point",[0,0],source_id,source_hash,{"runway_id":"09/27","threshold_id":"09"}),
        _feature("threshold_27","runway_threshold","Point",[0,100],source_id,source_hash,{"runway_id":"09/27","threshold_id":"27"}),
        _feature("taxiway_alpha","taxiway_centerline","LineString",[[0,10],[20,10]],source_id,source_hash,
                 {"width_m":12.0,"permissions":["aircraft","gse"],"directed":False}),
        _feature("service_road_alpha","service_road","LineString",[[20,10],[30,10]],source_id,source_hash,
                 {"width_m":5.0,"permissions":["gse"],"directed":False}),
        _feature("gse_link_alpha","gse_access_link","LineString",[[30,10],[40,10]],source_id,source_hash,
                 {"width_m":4.0,"permissions":["gse"],"directed":False}),
        _feature("stand_A1","aircraft_stand","Point",[20,10],source_id,source_hash,{"network_node_id":"n0002","stand_id":"A1"}),
        _feature("gse_depot_1","gse_depot","Point",[30,10],source_id,source_hash,{"network_node_id":"n0003"}),
        _feature("runway_crossing_1","runway_crossing","LineString",[[-2,10],[2,10]],source_id,source_hash,
                 {"permitted":True,"permission_source_id":source_id,"runway_id":"09/27"}),
        _feature("apron_alpha","apron","Polygon",[[[15,5],[25,5],[25,15],[15,15],[15,5]]],source_id,source_hash,
                 {"classification":"synthetic-test-apron"}),
        _feature("hold_short_09","hold_short_position","Point",[0,12],source_id,source_hash,{"runway_id":"09/27"}),
    ]
    relations=[{"relation_id":"runway_exit_09","relation_type":"runway_exit","from_node":"n0001","source_id":source_id,"source_feature_ids":["runway_09_27","taxiway_alpha"]},
               {"relation_id":"runway_entry_27","relation_type":"runway_entry","to_node":"n0001","source_id":source_id,"source_feature_ids":["runway_09_27","taxiway_alpha"]}]
    assessments=[{"assessment_id":"route-aircraft","assessment_type":"aircraft_route","required_clearance_m":2.0,"measured_clearance_m":2.5,
                  "source_id":source_id,"assessment_method":"synthetic fixture known-width calculation","feature_ids":["taxiway_alpha"],"simulation_approved":False},
                 {"assessment_id":"gse-to-aircraft","assessment_type":"aircraft_vs_gse","required_clearance_m":1.0,"measured_clearance_m":1.25,
                  "source_id":source_id,"assessment_method":"synthetic fixture known-clearance record","feature_ids":["gse_depot_1"],"simulation_approved":False}]
    if letter=="B":
        features[4]["coordinates"]=[[0,10],[15,10]]
        features[5]["coordinates"]=[[20,10],[30,10]]
        features[7]["properties"]["network_node_id"]="n0003"
        features[8]["properties"]["network_node_id"]="n0004"
    elif letter=="C":
        features[6]["coordinates"]=[[100,100],[110,100]]
        features[8]["coordinates"]=[100,100]
    elif letter=="D":
        features[9]["properties"]["permitted"]=False
        features[9]["properties"].pop("permission_source_id")
    elif letter=="E":
        assessments=[]
    elif letter=="F":
        features.append(_feature("taxiway_alpha","taxiway_centerline","LineString",[[1,1],[1,2]],source_id,source_hash))
    elif letter=="G":
        approximate_source="synthetic_goal46_fixture_G_approximate"
        approximate_hash=hashlib.sha256(b"explicit approximate test intrusion; not airport data").hexdigest()
        features.append(_feature("approx_route_attempt","taxiway_centerline","LineString",[[60,60],[70,60]],approximate_source,approximate_hash,
                                 {"permissions":["aircraft"],"width_m":None},authority="APPROXIMATE"))
        source_extra={"source_id":approximate_source,"source_type":"synthetic test-only bad input","source_file":"fixture-G-approximate-source.txt",
                      "source_sha256":approximate_hash,"publication_date":None,"extraction_method":"fixture bad-authority test",
                      "authority":"APPROXIMATE","notes":"Intentionally invalid for production routing."}
    elif letter=="H":
        features[4]["coordinates"]=[[0,10],[22,10]]
        features[4]["properties"]["permissions"]=["gse"]
        features[5]["coordinates"]=[[22,10],[30,10]]
        features[7]["coordinates"]=[22,10]
        features[7]["properties"]["network_node_id"]="n0002"
    else: source_extra=None
    source={"source_id":source_id,"source_type":"Synthetic Goal46 readiness fixture","source_file":"synthetic-fixture-source.json",
            "source_sha256":source_hash,"publication_date":"synthetic-fixture-v1","extraction_method":"deterministic fixture generator",
            "authority":"SYNTHETIC","notes":f"Fixture {letter}; synthetic test geometry only; not an airport dataset."}
    sources=[source]
    if letter=="G": sources.append(source_extra)
    dataset={"schema_version":SCHEMA_VERSION,"dataset_id":f"synthetic_goal46_fixture_{letter}",
             "airport":{"airport_id":f"SYNTHETIC-{letter}","name":f"Synthetic Goal46 Fixture {letter}","source_crs":"LOCAL_ENU",
                        "normalization_crs":"LOCAL_ENU","coordinate_units":"m","reference_point":{"coordinates_enu_m":[0,0],"label":"synthetic origin"}},
             "coordinate_system":{"source_crs":"LOCAL_ENU","normalization_crs":"LOCAL_ENU","units":"m","axes":["east","north","up"],
                                  "transformation":"Synthetic local coordinates copied as metres; no real location.","tolerance_m":0.001},
             "sources":sources,"features":features,"quarantined_features":[],"operational_graph":{"nodes":[],"edges":[]},
             "operational_relationships":relations,"clearance_assessments":assessments,"authority_policy":authority_policy(),
             "fixture_label":"SYNTHETIC TEST DATA ONLY; NOT AN AIRPORT; NEVER USE FOR REAL AIRPORT SAFETY CLAIMS."}
    build_graph(dataset,0.01)
    return dataset


def write_fixtures(folder: Path) -> dict[str, dict]:
    folder=Path(folder); folder.mkdir(parents=True,exist_ok=True)
    results={}
    for letter in "ABCDEFGH":
        dataset=make_fixture(letter)
        (folder/f"fixture_{letter.lower()}_synthetic.json").write_bytes(canonical_bytes(dataset))
        results[letter]=dataset
    return results
