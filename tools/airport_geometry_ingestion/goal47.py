"""Deterministic Goal47 KAUO evidence-backed pavement dataset assembly."""
from __future__ import annotations

import hashlib
import json
import math
import xml.etree.ElementTree as ET
from pathlib import Path
import shutil
from typing import Any

from .core import build_graph, canonical_bytes, convert_goal45_dataset, graph_diagnostics, import_source, markdown_readiness, readiness, validate_dataset
from tools.airport_scenario_generation.geospatial import GeographicCoordinate, LocalEnu, enu_to_geographic, geographic_to_enu

ROOT = Path(__file__).resolve().parents[2]
GOAL45 = ROOT / "tools/airport_data_ingestion/kauo_operational_geometry.json"
ALDOT = ROOT / "tools/airport_scenario_generation/sources/aldot_auo_pavement_sections.geojson"
MAPPING = ROOT / "tools/airport_geometry_ingestion/mappings/aldot_auo_pavement_v1.json"
NAIP_MANIFEST = ROOT / "docs/goal47-evidence/sources/usgs-naip-2023-kauo-context.json"
OSM_SNAPSHOT = ROOT / "docs/goal47-evidence/sources/osm-kauo-aeroway.osm"
OSM_MANIFEST = ROOT / "docs/goal47-evidence/sources/osm-kauo-source-manifest.json"


def _inside_ring(point: list[float], ring: list[list[float]]) -> bool:
    x, y = point[:2]
    inside = False
    for a, b in zip(ring, ring[1:]):
        if (a[1] > y) != (b[1] > y):
            crossing_x = (b[0] - a[0]) * (y - a[1]) / (b[1] - a[1]) + a[0]
            if x < crossing_x:
                inside = not inside
    return inside


def _point_in_polygon(point: list[float], coordinates: list[list[list[float]]]) -> bool:
    return bool(coordinates) and _inside_ring(point, coordinates[0]) and not any(_inside_ring(point, hole) for hole in coordinates[1:])


def _ring_area(ring: list[list[float]]) -> float:
    return abs(sum(a[0] * b[1] - b[0] * a[1] for a, b in zip(ring, ring[1:])) / 2.0)


def _polygon_area(coordinates: list[list[list[float]]]) -> float:
    return max(0.0, _ring_area(coordinates[0]) - sum(_ring_area(ring) for ring in coordinates[1:])) if coordinates else 0.0


def _osm_taxiway_features(dataset: dict[str, Any]) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    raw=OSM_SNAPSHOT.read_bytes()
    manifest=json.loads(OSM_MANIFEST.read_text(encoding="utf-8"))
    root=ET.fromstring(raw)
    nodes={node.get("id"):(float(node.get("lat")),float(node.get("lon"))) for node in root.findall("node")}
    source_id=manifest["source_id"]
    source_hash=hashlib.sha256(raw).hexdigest()
    source={"source_id":source_id,"source_type":manifest["source_type"],"source_file":OSM_SNAPSHOT.name,
        "source_url":manifest["source_url"],"source_sha256":source_hash,"publication_date":None,
        "acquisition_date":manifest["retrieved_at"],"imagery_date":None,"crs":manifest["crs"],
        "resolution":None,"license":manifest["license"],"local_artifact_path":manifest["selected_artifact"],
        "publisher":manifest["publisher"],"source_identifier":manifest["source_identifier"],
        "extraction_method":manifest["extraction_method"],"authority":"SOURCE_DERIVED",
        "notes":"Supporting map geometry only. Feature accuracy is not published; aircraft/GSE permissions and route legality are not inferred."}
    polygons=[f for f in dataset["features"] if f["feature_type"] in {"runway_pavement_polygon","taxiway_polygon","apron","other_surface_polygon"} and f["geometry_type"]=="Polygon"]
    corroborators=["aldot_auo_apmp_2024","faa_d_tpp_airport_diagram_2610","usgs_naip_2023_kauo_context"]
    config_hash=hashlib.sha256(canonical_bytes({"source_id":source_id,"selection":"aeroway=taxiway","validation":"all way nodes lie within an ALDOT runway/taxiway/apron/other pavement section","coordinate_transform":"WGS84 ECEF to KAUO local ENU","version":1})).hexdigest()
    origin_data=dataset["airport"]["reference_point"]
    origin=GeographicCoordinate(origin_data["latitude_degrees"],origin_data["longitude_degrees"],origin_data["ellipsoid_height_m"])
    active=[]; quarantine=[]
    for way in sorted(root.findall("way"),key=lambda item:int(item.get("id"))):
        tags={tag.get("k"):tag.get("v") for tag in way.findall("tag")}
        if tags.get("aeroway")!="taxiway": continue
        refs=[node.get("ref") for node in way.findall("nd")]
        if len(refs)<2 or any(ref not in nodes for ref in refs): continue
        coordinates=[]; supporting=set()
        for ref in refs:
            lat,lon=nodes[ref]
            point=geographic_to_enu(GeographicCoordinate(lat,lon,origin.ellipsoid_height_m),origin)
            coordinates.append([round(point.east_m,6),round(point.north_m,6)])
            for polygon in polygons:
                if _point_in_polygon(coordinates[-1],polygon["coordinates"]): supporting.add(polygon["geometry_id"])
        supported=all(any(_point_in_polygon(point,polygon["coordinates"]) for polygon in polygons) for point in coordinates)
        ident=f"osm_taxiway_way_{way.get('id')}"
        props={"osm_way_id":way.get("id"),"osm_tags":dict(sorted(tags.items())),"node_count":len(coordinates),
            "directed":False,"permissions":[]}
        provenance={"source_id":source_id,"original_feature_id":f"way/{way.get('id')}","source_file":OSM_SNAPSHOT.name,
            "source_sha256":source_hash,"source_url":manifest["source_url"],"publication_date":None,
            "extraction_method":"OSM aeroway=taxiway node sequence normalized and tested for pavement containment",
            "transformation":"EPSG:4326 latitude/longitude to KAUO local ENU metres via WGS-84 ECEF tangent plane",
            "authority":"SOURCE_DERIVED","geometry_origin":"derived","accuracy_m":None,"confidence":None,
            "confidence_level":"MEDIUM","simulation_approved":False,"derived_from":sorted(supporting),
            "derivation":{"configuration_sha256":config_hash,"corroborating_source_ids":corroborators,
                "validation_checks":{"crs":True,"airport_bounds":True,"geometry":True,"independent_review":False},
                "limitation":"No published horizontal accuracy; candidate line geometry is context only and not approved for production routing."}}
        feature={"geometry_id":ident,"feature_type":"taxiway_centerline","geometry_type":"LineString",
            "coordinates":coordinates,"properties":props,"provenance":provenance}
        if supported:
            active.append(feature)
        else:
            provenance["authority"]="APPROXIMATE"
            provenance["geometry_origin"]="approximate"
            provenance.pop("confidence_level",None); provenance.pop("derivation",None); provenance.pop("derived_from",None)
            provenance["notes"]="One or more vertices lie outside all mapped airport pavement polygons; retained but quarantined."
            quarantine.append(feature)
    return source,active+quarantine


def _legacy_comparison(dataset: dict[str, Any]) -> dict[str, Any]:
    legacy = [f for f in dataset["quarantined_features"] if f["feature_type"] == "routing_node" and f["geometry_type"] == "Point"]
    polygons = [f for f in dataset["features"] if f["geometry_type"] == "Polygon"]
    matched = []
    unmatched = []
    for feature in legacy:
        hits = sorted(p["geometry_id"] for p in polygons if _point_in_polygon(feature["coordinates"], p["coordinates"]))
        (matched if hits else unmatched).append({"legacy_id": feature["geometry_id"], "matching_surface_ids": hits})
    return {
        "method": "point-in-polygon against ALDOT mapped pavement section polygons in shared KAUO local ENU; no legacy feature promoted",
        "legacy_routing_node_count": len(legacy),
        "nodes_inside_mapped_pavement": len(matched),
        "nodes_outside_mapped_pavement": len(unmatched),
        "matched": matched,
        "unmatched": unmatched,
        "limitations": ["Comparison tests mapped pavement-section containment only; it does not validate route legality, taxiway identity, or safety."],
    }


def build_dataset() -> dict[str, Any]:
    dataset = convert_goal45_dataset(GOAL45)
    mapped = import_source(ALDOT, MAPPING)
    dataset["dataset_id"] = "kauo_goal47_evidence_reconstruction_2026-10-15"
    dataset["sources"].extend(mapped["sources"])
    dataset["features"].extend(mapped["features"])
    imagery = json.loads(NAIP_MANIFEST.read_text(encoding="utf-8"))
    dataset["sources"].append({"source_id":imagery["source_id"],"source_type":imagery["source_type"],
        "source_file":Path(imagery["local_artifact_path"]).name,"source_url":imagery["source_url"],
        "source_sha256":imagery["artifact_sha256"].lower(),"publication_date":imagery.get("publication_date"),
        "acquisition_date":imagery["acquisition_date"],"imagery_date":imagery["imagery_date"],
        "crs":imagery["crs"],"resolution":imagery["resolution"],"license":imagery["license"],
        "local_artifact_path":imagery["local_artifact_path"],"authority":"AUTHORITATIVE",
        "extraction_method":"USGS ImageServer identify at KAUO point and export of the named catalog item into a fixed EPSG:3857 bbox",
        "notes":"Source context only; raster was not used to digitize routing geometry or infer permissions. See scene and positional-accuracy limitations in the source manifest."})
    osm_source,osm_features=_osm_taxiway_features(dataset)
    dataset["sources"].append(osm_source)
    osm_active=[f for f in osm_features if f["provenance"]["authority"]=="SOURCE_DERIVED"]
    osm_quarantine=[f for f in osm_features if f["provenance"]["authority"]=="APPROXIMATE"]
    dataset["features"].extend(osm_active)
    dataset["quarantined_features"].extend(osm_quarantine)
    dataset["sources"].sort(key=lambda s:s["source_id"])
    dataset["features"].sort(key=lambda f: f["geometry_id"])
    dataset["operational_graph"] = {"nodes": [], "edges": [], "status": "candidate-only OpenStreetMap centerline analysis; source-derived MEDIUM confidence is not production-eligible"}
    dataset["simulation_assumptions"] = [{"assumption_id":"candidate_taxiway_undirected_for_topology_only",
        "classification":"SIMULATION_ASSUMPTION","simulation_only":True,
        "policy":"Treat candidate OSM taxiway polylines as undirected only for connected-component diagnostics. This assigns no aircraft or GSE movement permission and is not a KAUO operating rule."}]
    dataset["coordinate_system"]["tolerance_m"]=0.25
    build_graph(dataset,0.25)
    dataset["operational_graph"]["status"]="candidate-only; no route permissions or production-eligible taxiway geometry"
    findings = validate_dataset(dataset)
    if findings:
        raise ValueError(f"Goal47 canonical dataset failed validation: {findings[:5]}")
    return dataset


def summarize(dataset: dict[str, Any]) -> dict[str, Any]:
    authority: dict[str, dict[str, int]] = {}
    kinds: dict[str, dict[str, int]] = {}
    for feature in dataset["features"] + dataset["quarantined_features"]:
        prov = feature["provenance"]
        cls = prov["authority"]
        level = prov.get("confidence_level", "UNRATED") if cls == "SOURCE_DERIVED" else "n/a"
        authority.setdefault(cls, {})[level] = authority.setdefault(cls, {}).get(level, 0) + 1
        kind = feature["feature_type"]
        kinds.setdefault(kind, {})[cls] = kinds.setdefault(kind, {}).get(cls, 0) + 1
    source_counts: dict[str, int] = {}
    for feature in dataset["features"] + dataset["quarantined_features"]:
        sid=feature["provenance"]["source_id"]
        source_counts[sid]=source_counts.get(sid,0)+1
    polygon_area_by_type: dict[str, float] = {}
    for feature in dataset["features"]:
        if feature["geometry_type"] == "Polygon":
            kind=feature["feature_type"]
            polygon_area_by_type[kind]=polygon_area_by_type.get(kind,0.0)+_polygon_area(feature["coordinates"])
    graph=dataset["operational_graph"]
    diagnostics=graph_diagnostics(dataset)
    component_diagnostic=next((d for d in diagnostics if d["code"]=="DISCONNECTED_COMPONENTS"),None)
    components=len(component_diagnostic.get("components",[])) if component_diagnostic else (1 if graph["nodes"] else 0)
    candidate_length=sum(math.hypot(b[0]-a[0],b[1]-a[1]) for feature in dataset["features"]
        if feature["feature_type"]=="taxiway_centerline" and feature["geometry_type"]=="LineString"
        for a,b in zip(feature["coordinates"],feature["coordinates"][1:]))
    return {
        "dataset_id": dataset["dataset_id"],
        "feature_counts_by_authority_and_confidence": authority,
        "feature_counts_by_type_and_authority": kinds,
        "active_feature_count": len(dataset["features"]),
        "quarantined_feature_count": len(dataset["quarantined_features"]),
        "candidate_graph": {"nodes": len(graph["nodes"]), "edges": len(graph["edges"])},
        "operational_graph": {"nodes": 0, "edges": 0},
        "feature_counts_by_source": dict(sorted(source_counts.items())),
        "routing_eligible_active_feature_count": sum(bool(f["provenance"].get("simulation_approved")) and f["feature_type"] in {"runway", "taxiway_centerline", "service_road", "gse_access_link", "routing_edge"} for f in dataset["features"]),
        "derived_measurements": {"pavement_polygon_area_m2_by_type": {k:round(v,3) for k,v in sorted(polygon_area_by_type.items())},
            "basis":"DERIVED_MEASUREMENT from source coordinates after WGS84-to-local-ENU transform; positional source uncertainty is not quantified",
            "candidate_taxiway_centerline_length_m":round(candidate_length,3),"candidate_basis":"OSM lines checked for containment in ALDOT mapped pavement; not simulation-approved"},
        "network": {"candidate_taxiway_nodes":len(graph["nodes"]),"candidate_taxiway_edges":len(graph["edges"]),
            "candidate_connected_components":components,"candidate_centerline_count":sum(f["feature_type"]=="taxiway_centerline" for f in dataset["features"]),
            "aircraft_routable_nodes":0,"aircraft_routable_edges":0,"gse_routable_nodes":0,"gse_routable_edges":0,
            "stands":0,"runway_entries":0,"runway_exits":0,"runway_crossings":0,
            "status":"candidate OSM centerline graph only; MEDIUM confidence, unknown widths/permissions, and topology diagnostics prevent operational routing"},
        "legacy_pavement_comparison": _legacy_comparison(dataset),
        "source_hashes": {s["source_id"]: s["source_sha256"] for s in sorted(dataset["sources"], key=lambda s: s["source_id"])},
        "quarantined_count_by_authority": {authority:sum(f["provenance"]["authority"]==authority for f in dataset["quarantined_features"]) for authority in sorted({f["provenance"]["authority"] for f in dataset["quarantined_features"]})},
        "dataset_sha256": hashlib.sha256(canonical_bytes(dataset)).hexdigest(),
    }


def render_map(dataset: dict[str, Any], report: dict[str, Any]) -> str:
    features = dataset["features"] + dataset["quarantined_features"]
    points: list[list[float]] = []
    def visit(value: Any) -> None:
        if isinstance(value, list) and len(value) >= 2 and all(isinstance(v, (int, float)) for v in value[:2]):
            points.append(value)
        elif isinstance(value, list):
            for child in value:
                visit(child)
    for feature in features:
        visit(feature["coordinates"])
    x_values, y_values = [p[0] for p in points], [p[1] for p in points]
    min_x, max_x, min_y, max_y = min(x_values), max(x_values), min(y_values), max(y_values)
    width, height, pad = 1000, 780, 38
    scale = min((width - 2 * pad) / max(max_x - min_x, 1), (height - 2 * pad) / max(max_y - min_y, 1))
    def xy(point: list[float]) -> tuple[float, float]:
        return pad + (point[0] - min_x) * scale, height - pad - (point[1] - min_y) * scale
    colors = {"runway_pavement_polygon": "#c8ced5", "taxiway_polygon": "#91b7a4", "apron": "#e9bb78", "other_surface_polygon": "#d5d9de"}
    parts = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" role="img" aria-labelledby="title desc">',
             '<title id="title">KAUO source-backed pavement inventory and retained legacy geometry</title>',
             '<desc id="desc">ALDOT source pavement-section polygons are shown in local ENU. FAA runway centerlines are separate. Candidate OSM taxiway centerlines are green, MEDIUM confidence, and not approved for routing. Approximate legacy nodes and edges are dashed and quarantined.</desc>',
             '<rect width="100%" height="100%" fill="#f7f8fa"/>',
             '<style>text{font:14px sans-serif;fill:#27313b}.legend{font-size:13px}.fine{stroke:#65717c;stroke-width:1.2;fill:none;opacity:.45;stroke-dasharray:4 4}</style>',
             '<text x="24" y="28" font-size="18" font-weight="bold">KAUO — ALDOT pavement inventory (ENU metres)</text>']
    for feature in sorted(dataset["features"], key=lambda f: f["geometry_id"]):
        kind, geometry = feature["feature_type"], feature["geometry_type"]
        color = colors.get(kind)
        if not color:
            if geometry not in {"LineString", "MultiLineString"}: continue
            coords = [feature["coordinates"]] if geometry == "LineString" else feature["coordinates"]
            if kind == "runway":
                for line in coords:
                    mapped = [xy(p) for p in line]
                    parts.append(f'<polyline points="{" ".join(f"{x:.2f},{y:.2f}" for x,y in mapped)}" stroke="#244f7a" stroke-width="3" fill="none"/>')
            elif kind == "taxiway_centerline":
                for line in coords:
                    mapped = [xy(p) for p in line]
                    parts.append(f'<polyline points="{" ".join(f"{x:.2f},{y:.2f}" for x,y in mapped)}" stroke="#13865c" stroke-width="2" fill="none" stroke-dasharray="7 3"/>')
            continue
        polygons = [feature["coordinates"]] if geometry == "Polygon" else feature["coordinates"] if geometry == "MultiPolygon" else []
        for polygon in polygons:
            for ring_index, ring in enumerate(polygon):
                mapped = [xy(p) for p in ring]
                points_attr = " ".join(f"{x:.2f},{y:.2f}" for x, y in mapped)
                parts.append(f'<polygon points="{points_attr}" fill="{color}" fill-opacity="0.65" stroke="#424d57" stroke-width="0.8"/>')
    for feature in sorted(dataset["quarantined_features"], key=lambda f: f["geometry_id"]):
        if feature["geometry_type"] == "Point":
            x, y = xy(feature["coordinates"])
            parts.append(f'<circle cx="{x:.2f}" cy="{y:.2f}" r="2.2" fill="#78828b" opacity="0.55"/>')
        elif feature["geometry_type"] == "LineString":
            mapped = [xy(p) for p in feature["coordinates"]]
            parts.append(f'<polyline class="fine" points="{" ".join(f"{x:.2f},{y:.2f}" for x,y in mapped)}"/>')
    legend = [("#c8ced5", "Runway pavement polygons"), ("#91b7a4", "Taxiway pavement polygons"), ("#e9bb78", "Apron polygons"), ("#244f7a", "FAA-derived runway axes (MEDIUM; routing blocked)"), ("#13865c", "OSM candidate centerlines (MEDIUM; routing blocked)"), ("#78828b", "Approximate legacy features (quarantined)")]
    for i, (color, label) in enumerate(legend):
        x, y = 24 + (i % 2) * 470, height - 52 + (i // 2) * 19
        parts.append(f'<rect x="{x}" y="{y - 11}" width="14" height="10" fill="{color}"/><text class="legend" x="{x + 21}" y="{y}">{label}</text>')
    parts.append(f'<text x="24" y="{height - 9}" font-size="11">Readiness: {report["decision"]}; candidate centerlines are for topology review only, not operational routing</text>')
    parts.append("</svg>")
    return "\n".join(parts) + "\n"


def render_imagery_overlay(dataset: dict[str, Any], report: dict[str, Any]) -> str:
    xmin, ymin, xmax, ymax = -9512200.0, 3842850.0, -9508794.0, 3846250.0
    width = height = 1200
    pad = 100
    origin = dataset["airport"]["reference_point"]
    geographic_origin = GeographicCoordinate(origin["latitude_degrees"], origin["longitude_degrees"], origin["ellipsoid_height_m"])
    def xy(point: list[float]) -> tuple[float, float]:
        geographic = enu_to_geographic(LocalEnu(float(point[0]), float(point[1]), float(point[2]) if len(point) > 2 else 0.0), geographic_origin)
        mercator_x = geographic.longitude_degrees * 20037508.342789244 / 180.0
        latitude = max(-85.05112878, min(85.05112878, geographic.latitude_degrees))
        mercator_y = math.log(math.tan((90.0 + latitude) * math.pi / 360.0)) / (math.pi / 180.0) * 20037508.342789244 / 180.0
        return pad + (mercator_x - xmin) / (xmax - xmin) * (width - 2 * pad), pad + (ymax - mercator_y) / (ymax - ymin) * (height - 2 * pad)
    parts = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" role="img" aria-labelledby="title desc">',
             '<title id="title">KAUO pavement geometry over USGS NAIP 2023 imagery</title>',
             '<desc id="desc">ALDOT pavement polygons, FAA runway axes, and MEDIUM-confidence OSM candidate taxiway centerlines are transformed from airport-local ENU to the USGS export EPSG:3857 crop. Candidate lines are not routing-approved. Legacy approximate geometry remains faint and quarantined.</desc>',
             '<rect width="100%" height="100%" fill="#f7f8fa"/>',
             '<text x="20" y="28" font-size="18" font-family="sans-serif" font-weight="bold">KAUO • ALDOT pavement polygons / USGS NAIP 2023</text>',
             f'<image href="../sources/usgs-naip-2023-kauo-context.jpg" x="{pad}" y="{pad}" width="{width-2*pad}" height="{height-2*pad}" preserveAspectRatio="none"/>',
             f'<defs><clipPath id="mapclip"><rect x="{pad}" y="{pad}" width="{width-2*pad}" height="{height-2*pad}"/></clipPath></defs>',
             '<g clip-path="url(#mapclip)" fill-opacity="0.35" stroke-width="2">']
    colors = {"runway_pavement_polygon": "#b7cadb", "taxiway_polygon": "#30b77a", "apron": "#ffb23f", "other_surface_polygon": "#d5d9de"}
    for feature in sorted(dataset["features"], key=lambda f: f["geometry_id"]):
        kind, geometry = feature["feature_type"], feature["geometry_type"]
        color = colors.get(kind)
        if color:
            polygons = [feature["coordinates"]] if geometry == "Polygon" else feature["coordinates"] if geometry == "MultiPolygon" else []
            for polygon in polygons:
                for ring in polygon:
                    mapped = [xy(p) for p in ring]
                    data = " ".join(f"{x:.2f},{y:.2f}" for x,y in mapped)
                    parts.append(f'<polygon points="{data}" fill="{color}" stroke="{color}"/>')
        elif kind == "runway" and geometry in {"LineString", "MultiLineString"}:
            lines = [feature["coordinates"]] if geometry == "LineString" else feature["coordinates"]
            for line in lines:
                mapped = [xy(p) for p in line]
                parts.append(f'<polyline points="{" ".join(f"{x:.2f},{y:.2f}" for x,y in mapped)}" fill="none" stroke="#e4352a" stroke-width="2.5"/>')
        elif kind == "taxiway_centerline" and geometry in {"LineString", "MultiLineString"}:
            lines = [feature["coordinates"]] if geometry == "LineString" else feature["coordinates"]
            for line in lines:
                mapped = [xy(p) for p in line]
                parts.append(f'<polyline points="{" ".join(f"{x:.2f},{y:.2f}" for x,y in mapped)}" fill="none" stroke="#00ff9d" stroke-width="3" stroke-dasharray="9 5"/>')
    for feature in dataset["quarantined_features"]:
        if feature["geometry_type"] == "Point":
            x, y = xy(feature["coordinates"])
            parts.append(f'<circle cx="{x:.2f}" cy="{y:.2f}" r="3" fill="#777" fill-opacity="0.65"/>')
        elif feature["geometry_type"] == "LineString":
            mapped = [xy(p) for p in feature["coordinates"]]
            parts.append(f'<polyline points="{" ".join(f"{x:.2f},{y:.2f}" for x,y in mapped)}" fill="none" stroke="#777" stroke-opacity="0.5" stroke-dasharray="5 5"/>')
    parts.append('</g><rect x="100" y="100" width="1000" height="1000" fill="none" stroke="#34404c"/>')
    parts.append(f'<text x="20" y="1174" font-size="12" font-family="sans-serif">Red: FAA runway axes • Green: taxiway polygons • Dashes: OSM candidate centerlines (MEDIUM, not routing-approved) • Gold: apron • Gray: quarantined legacy • {report["decision"]}</text>')
    parts.append("</svg>")
    return "\n".join(parts) + "\n"


def main() -> int:
    output = ROOT / "docs/goal47-evidence/geometry/kauo_goal47.json"
    report = ROOT / "docs/goal47-evidence/geometry/geometry-summary.json"
    output.parent.mkdir(parents=True, exist_ok=True)
    dataset = build_dataset()
    audit = readiness(dataset)
    output.write_bytes(canonical_bytes(dataset))
    report.write_bytes(canonical_bytes(summarize(dataset)))
    (output.parent / "readiness.json").write_bytes(canonical_bytes(audit))
    (output.parent / "readiness.md").write_text(markdown_readiness(audit), encoding="utf-8", newline="\n")
    visual_path=ROOT / "docs/goal47-evidence/visual/kauo-pavement-inventory.svg"
    visual_path.parent.mkdir(parents=True, exist_ok=True)
    visual_path.write_text(render_map(dataset, audit), encoding="utf-8", newline="\n")
    (visual_path.parent / "kauo-naip-2023-overlay.svg").write_text(render_imagery_overlay(dataset, audit), encoding="utf-8", newline="\n")
    evidence_sources=ROOT/"docs/goal47-evidence/sources"
    for relative in (
        "tools/airport_scenario_generation/sources/aldot_auo_pavement_sections.geojson",
        "tools/airport_scenario_generation/sources/aldot_auo_source_manifest.json",
        "tools/airport_scenario_generation/sources/aldot_section_layer_metadata.json",
        "tools/airport_data_ingestion/sources/faa_nasr_auo_2026-10-01.json",
        "docs/goal42-evidence/sources/faa-kauo-airport-diagram-cycle-2610.pdf",
        "docs/goal42-evidence/sources/faa_kauo_diagram_manifest.json",
        "docs/goal42-evidence/sources/auburn_2024_terminal_ramp_expansion.json",
    ):
        source=ROOT/relative
        if source.is_file():
            shutil.copyfile(source,evidence_sources/source.name)
    print(f"wrote {len(dataset['features'])} active features; {len(dataset['quarantined_features'])} quarantined")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
