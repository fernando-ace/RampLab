"""Canonical airport geometry model, import adapters, and fail-closed audits.

Coordinates in canonical datasets are local ENU metres (+X east, +Y north).
Import mappings are explicit: adapters never infer source columns or authority.
"""
from __future__ import annotations

import csv
import hashlib
import io
import json
import math
import re
import zipfile
from pathlib import Path
from typing import Any
import xml.etree.ElementTree as ET

from tools.airport_scenario_generation.geospatial import GeographicCoordinate, geographic_to_enu, rotate_local_grid_to_enu
from . import SCHEMA_VERSION

AUTHORITIES = {"AUTHORITATIVE", "SOURCE_DERIVED", "OPERATOR_PROVIDED", "SURVEYED", "APPROXIMATE", "SYNTHETIC", "UNKNOWN"}
PRODUCTION_AUTHORITIES = {"AUTHORITATIVE", "SOURCE_DERIVED", "OPERATOR_PROVIDED", "SURVEYED"}
GEOMETRIES = {"Point", "MultiPoint", "LineString", "MultiLineString", "Polygon", "MultiPolygon"}
FEATURE_TYPES = {
    "airport_reference_point", "runway", "runway_threshold", "taxiway_centerline", "taxiway_polygon",
    "taxiway_intersection", "hold_short_position", "apron", "aircraft_stand", "service_road",
    "gse_staging_area", "gse_access_link", "gse_depot", "runway_crossing", "restricted_area",
    "protected_area", "routing_node", "routing_edge", "other",
}
REQUIRED_PROVENANCE = ("source_id", "original_feature_id", "extraction_method", "transformation", "authority", "geometry_origin")


class GeometryInputError(ValueError):
    """Expected user-facing data or mapping error."""


def canonical_bytes(value: Any) -> bytes:
    return (json.dumps(value, ensure_ascii=False, sort_keys=True, indent=2, allow_nan=False) + "\n").encode("utf-8")


def _sha(raw: bytes) -> str:
    return hashlib.sha256(raw).hexdigest()


def _finite_number(value: Any) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(float(value))


def _read_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8-sig"), parse_constant=lambda v: (_ for _ in ()).throw(ValueError(f"invalid JSON number {v}")))
    except (OSError, UnicodeError, json.JSONDecodeError, ValueError) as exc:
        raise GeometryInputError(f"{path.name}: {exc}") from None


def _load_mapping(path: Path) -> dict[str, Any]:
    value = _read_json(path)
    if not isinstance(value, dict):
        raise GeometryInputError("mapping must be a JSON object")
    for key in ("airport", "source", "source_crs", "source_units", "field_mapping"):
        if key not in value:
            raise GeometryInputError(f"mapping is missing required key {key!r}")
    if not isinstance(value["airport"], dict) or not value["airport"].get("airport_id"):
        raise GeometryInputError("mapping.airport.airport_id is required")
    if not isinstance(value["source"], dict) or not value["source"].get("source_id") or not value["source"].get("authority"):
        raise GeometryInputError("mapping.source.source_id and mapping.source.authority are required")
    if value["source"]["authority"] not in AUTHORITIES:
        raise GeometryInputError(f"unsupported authority {value['source']['authority']!r}")
    if not isinstance(value["field_mapping"], dict):
        raise GeometryInputError("mapping.field_mapping must be an object")
    return value


def _property(properties: dict[str, Any], field: str | None, *, required: bool = False, label: str = "field") -> Any:
    if not field:
        if required:
            raise GeometryInputError(f"mapping must name a {label}")
        return None
    if field not in properties:
        raise GeometryInputError(f"mapped {label} {field!r} is absent from source feature")
    return properties[field]


def _normalize_position(position: Any, crs: str, units: str, origin: dict[str, Any] | None,
                        local_transform: dict[str, Any] | None = None) -> list[float]:
    if not isinstance(position, (tuple, list)) or len(position) < 2 or not all(_finite_number(v) for v in position):
        raise GeometryInputError("each coordinate must contain at least two finite numbers")
    crs_key = crs.strip().upper()
    if crs_key in {"EPSG:4326", "EPSG:4979", "OGC:CRS84", "WGS84", "WGS 84"}:
        if origin is None:
            raise GeometryInputError("WGS84 import requires mapping.origin with latitude_degrees, longitude_degrees, and ellipsoid_height_m")
        lon, lat = float(position[0]), float(position[1])
        height = float(position[2]) if len(position) > 2 else float(origin["ellipsoid_height_m"])
        converted = geographic_to_enu(
            GeographicCoordinate(lat, lon, height),
            GeographicCoordinate(float(origin["latitude_degrees"]), float(origin["longitude_degrees"]), float(origin["ellipsoid_height_m"])),
        )
        return [round(converted.east_m, 6), round(converted.north_m, 6)] + ([round(converted.up_m, 6)] if len(position) > 2 else [])
    if crs_key in {"LOCAL_ENU", "RAMPLAB_LOCAL_ENU"}:
        factors = {"m": 1.0, "meter": 1.0, "meters": 1.0, "ft": 0.3048, "feet": 0.3048}
        unit = units.lower()
        if unit not in factors:
            raise GeometryInputError("local source units must be explicitly m or ft")
        return [round(float(position[0]) * factors[unit], 6), round(float(position[1]) * factors[unit], 6)] + ([round(float(position[2]) * factors[unit], 6)] if len(position) > 2 else [])
    if crs_key in {"LOCAL_SIMULATOR", "LOCAL_GRID"}:
        if not isinstance(local_transform,dict) or not _finite_number(local_transform.get("positive_x_true_bearing_degrees")):
            raise GeometryInputError("LOCAL_SIMULATOR requires local_transform.positive_x_true_bearing_degrees; simulator axes are never guessed")
        factors={"m":1.0,"meter":1.0,"meters":1.0,"ft":0.3048,"feet":0.3048}
        if units.lower() not in factors: raise GeometryInputError("local source units must be explicitly m or ft")
        scale=factors[units.lower()]
        converted=rotate_local_grid_to_enu(float(position[0])*scale,float(position[1])*scale,
                     positive_x_true_bearing_degrees=float(local_transform["positive_x_true_bearing_degrees"]),
                     origin_offset_east_m=float(local_transform.get("origin_offset_east_m",0.0)),
                     origin_offset_north_m=float(local_transform.get("origin_offset_north_m",0.0)))
        return [round(converted.east_m,6),round(converted.north_m,6)]+([round(float(position[2])*scale,6)] if len(position)>2 else [])
    raise GeometryInputError(f"unsupported source CRS {crs!r}; supported: EPSG:4326, EPSG:4979, OGC:CRS84, LOCAL_ENU")


def _map_coordinates(value: Any, geometry_type: str, crs: str, units: str, origin: dict[str, Any] | None,
                     local_transform: dict[str, Any] | None = None) -> Any:
    if geometry_type not in GEOMETRIES:
        raise GeometryInputError(f"unsupported geometry type {geometry_type!r}")
    if geometry_type in {"Point", "MultiPoint"}:
        if geometry_type == "Point":
            return _normalize_position(value, crs, units, origin,local_transform)
        return [_normalize_position(p, crs, units, origin,local_transform) for p in value]
    if geometry_type == "LineString":
        if not isinstance(value, list) or len(value) < 2:
            raise GeometryInputError("LineString requires at least two positions")
        return [_normalize_position(p, crs, units, origin,local_transform) for p in value]
    if geometry_type == "MultiLineString":
        return [_map_coordinates(line, "LineString", crs, units, origin,local_transform) for line in value]
    if geometry_type == "Polygon":
        return [_map_coordinates(ring, "LineString", crs, units, origin,local_transform) for ring in value]
    if geometry_type == "MultiPolygon":
        return [_map_coordinates(poly, "Polygon", crs, units, origin,local_transform) for poly in value]
    raise GeometryInputError(f"unsupported geometry type {geometry_type!r}")


def _geometry_type_for_feature_type(kind: str) -> set[str]:
    if kind in {"runway", "taxiway_centerline", "service_road", "gse_access_link", "routing_edge", "runway_crossing"}:
        return {"LineString", "MultiLineString"}
    if kind in {"taxiway_polygon", "apron", "gse_staging_area", "restricted_area", "protected_area"}:
        return {"Polygon", "MultiPolygon"}
    return {"Point", "MultiPoint"}


def _source_entry(path: Path, raw: bytes, config: dict[str, Any], source_type: str) -> dict[str, Any]:
    source = config["source"]
    return {
        "source_id": source["source_id"], "source_type": source.get("source_type", source_type),
        "source_file": path.name, "source_url": source.get("source_url"),
        "source_sha256": _sha(raw), "publication_date": source.get("publication_date"),
        "extraction_method": source.get("extraction_method", f"explicit {source_type} field mapping"),
        "authority": source["authority"], "notes": source.get("notes", ""),
    }


def _feature(raw_id: Any, kind: Any, geometry_type: str, coords: Any, props: dict[str, Any], config: dict[str, Any],
             source_entry: dict[str, Any], *, conversion: str) -> dict[str, Any]:
    if not raw_id or not str(raw_id).strip():
        raise GeometryInputError("feature identifier is empty")
    kind = str(kind).strip()
    if kind not in FEATURE_TYPES:
        raise GeometryInputError(f"unsupported feature_type {kind!r}; use one of the documented schema values")
    if geometry_type not in _geometry_type_for_feature_type(kind):
        raise GeometryInputError(f"{kind} cannot use {geometry_type} geometry")
    fields = config["field_mapping"]
    auth_field = fields.get("authority")
    authority = props.get(auth_field, source_entry["authority"]) if auth_field else source_entry["authority"]
    if authority not in AUTHORITIES:
        raise GeometryInputError(f"feature {raw_id}: unsupported authority {authority!r}")
    excluded = {fields.get(k) for k in ("id", "feature_type", "authority", "coordinates", "geometry_type") if fields.get(k)}
    clean_props = {str(k): v for k, v in props.items() if k not in excluded}
    crs = config["source_crs"]
    transform = f"{crs} to local ENU metres; {conversion}; axis order explicitly configured by source format; parameters={json.dumps(config.get('local_transform'),sort_keys=True)}"
    provenance = {
        "source_id": source_entry["source_id"], "original_feature_id": str(raw_id),
        "source_file": source_entry["source_file"], "source_sha256": source_entry["source_sha256"],
        "source_url": source_entry.get("source_url"), "publication_date": source_entry.get("publication_date"),
        "extraction_method": source_entry["extraction_method"], "transformation": transform,
        "authority": authority, "geometry_origin": "direct", "accuracy_m": props.get("accuracy_m"),
        "confidence":props.get("confidence"),
        "simulation_approved": props.get("simulation_approved", authority in PRODUCTION_AUTHORITIES),
    }
    if authority in {"APPROXIMATE", "SYNTHETIC", "UNKNOWN"}:
        provenance["simulation_approved"] = False
    if "derived_from" in props:
        provenance["derived_from"] = props["derived_from"]
        provenance["geometry_origin"] = "derived"
    if "inferred" in props and props["inferred"]:
        provenance["geometry_origin"] = "inferred"
    return {"geometry_id": str(raw_id).strip(), "feature_type": kind, "geometry_type": geometry_type,
            "coordinates": _map_coordinates(coords, geometry_type, crs, config["source_units"], config.get("origin"),config.get("local_transform")),
            "properties": clean_props, "provenance": provenance}


def _raw_features(path: Path, raw: bytes, config: dict[str, Any]) -> tuple[list[tuple[Any, Any, str, Any, dict[str, Any]]], str]:
    suffix = path.suffix.lower()
    fields = config["field_mapping"]
    if suffix in {".json", ".geojson"}:
        data = json.loads(raw.decode("utf-8-sig"), parse_constant=lambda v: (_ for _ in ()).throw(ValueError(f"invalid JSON number {v}")))
        if isinstance(data, dict) and data.get("type") == "FeatureCollection":
            items = data.get("features")
        elif isinstance(data, dict) and data.get("schema_version") == SCHEMA_VERSION:
            raise GeometryInputError("canonical geometry input should be validated directly; use `validate` or `convert` without a mapping")
        else:
            raise GeometryInputError("GeoJSON input must be a FeatureCollection")
        if not isinstance(items, list):
            raise GeometryInputError("GeoJSON FeatureCollection.features must be an array")
        result = []
        for index, item in enumerate(items, 1):
            if not isinstance(item, dict) or not isinstance(item.get("geometry"), dict):
                raise GeometryInputError(f"GeoJSON feature {index} must include a geometry")
            props = item.get("properties") or {}
            if not isinstance(props, dict):
                raise GeometryInputError(f"GeoJSON feature {index} properties must be an object")
            identity = _property(props, fields.get("id"), required=True, label="id field")
            kind = _property(props, fields.get("feature_type"), required=True, label="feature_type field")
            result.append((identity, kind, item["geometry"].get("type"), item["geometry"].get("coordinates"), props))
        return result, "GeoJSON RFC 7946 coordinates [longitude, latitude, altitude metres]"
    if suffix == ".csv":
        text = raw.decode("utf-8-sig")
        reader = csv.DictReader(io.StringIO(text, newline=""), strict=True)
        if not reader.fieldnames or len(reader.fieldnames) != len(set(reader.fieldnames)):
            raise GeometryInputError("CSV requires unique headers")
        if not fields.get("id") or not fields.get("feature_type"):
            raise GeometryInputError("CSV mapping must explicitly name id and feature_type fields")
        coordinate_spec=fields.get("coordinate_fields")
        if not fields.get("coordinates") and not isinstance(coordinate_spec,dict):
            raise GeometryInputError("CSV mapping must name a JSON coordinates field or explicit coordinate_fields")
        result = []
        for index, row in enumerate(reader, 2):
            if None in row:
                raise GeometryInputError(f"CSV row {index} contains more values than headers")
            if fields.get("coordinates"):
                if not fields.get("geometry_type"):
                    raise GeometryInputError("CSV coordinates-field mapping must explicitly name geometry_type")
                try:
                    coords = json.loads(row[fields["coordinates"]], parse_constant=lambda v: (_ for _ in ()).throw(ValueError(f"invalid JSON number {v}")))
                except (json.JSONDecodeError, ValueError) as exc:
                    raise GeometryInputError(f"CSV row {index} coordinates must be a JSON coordinate array: {exc}") from None
                geometry_type=row.get(fields["geometry_type"])
            else:
                point_fields=coordinate_spec.get("point")
                edge_fields=coordinate_spec.get("edge")
                if point_fields and all(k in point_fields for k in ("x","y")):
                    try: point=[float(row[point_fields["x"]]),float(row[point_fields["y"]])]
                    except (ValueError,TypeError,KeyError): raise GeometryInputError(f"CSV row {index} has invalid mapped point coordinates") from None
                    coords=point; geometry_type="Point"
                elif edge_fields and all(k in edge_fields for k in ("start_x","start_y","end_x","end_y")):
                    try: coords=[[float(row[edge_fields[k]]) for k in ("start_x","start_y")],
                                 [float(row[edge_fields[k]]) for k in ("end_x","end_y")]]
                    except (ValueError,TypeError,KeyError): raise GeometryInputError(f"CSV row {index} has invalid mapped edge coordinates") from None
                    geometry_type="LineString"
                else:
                    raise GeometryInputError("coordinate_fields must explicitly provide point x/y and/or edge start_x/start_y/end_x/end_y mappings")
            result.append((row.get(fields["id"]), row.get(fields["feature_type"]), geometry_type, coords, row))
        return result, "CSV coordinates JSON field mapped explicitly; coordinate axis order follows source_crs"
    if suffix in {".kml", ".kmz"}:
        if suffix == ".kmz":
            try:
                with zipfile.ZipFile(io.BytesIO(raw)) as archive:
                    candidates = sorted(name for name in archive.namelist() if name.lower().endswith(".kml"))
                    if not candidates: raise GeometryInputError("KMZ contains no KML document")
                    kml_raw = archive.read(candidates[0])
            except (zipfile.BadZipFile, KeyError) as exc:
                raise GeometryInputError(f"invalid KMZ archive: {exc}") from None
        else:
            kml_raw = raw
        try:
            root = ET.fromstring(kml_raw)
        except ET.ParseError as exc:
            raise GeometryInputError(f"invalid KML XML: {exc}") from None
        def local(tag: str) -> str: return tag.rsplit("}", 1)[-1]
        result = []
        for index, placemark in enumerate((node for node in root.iter() if local(node.tag) == "Placemark"), 1):
            props: dict[str, Any] = {}
            geom = None
            for child in placemark.iter():
                name = local(child.tag)
                if name == "name" and child.text: props.setdefault("name", child.text.strip())
                elif name == "SimpleData" and child.attrib.get("name"):
                    props[child.attrib["name"]] = child.text.strip() if child.text else ""
                elif name == "Data" and child.attrib.get("name"):
                    value = next((x.text for x in child if local(x.tag) == "value"), "")
                    props[child.attrib["name"]] = (value or "").strip()
                elif name in {"Point", "LineString", "Polygon"} and geom is None:
                    coords_node = next((x for x in child.iter() if local(x.tag) == "coordinates"), None)
                    if coords_node is None or not coords_node.text: continue
                    tuples = [[float(v) for v in token.split(",")] for token in coords_node.text.split()]
                    if any(len(v)>2 for v in tuples): props["source_kml_altitude_m"]=[v[2] for v in tuples if len(v)>2]
                    horizontal=[v[:2] for v in tuples]
                    if name == "Point": geom = ("Point", horizontal[0])
                    elif name == "LineString": geom = ("LineString", horizontal)
                    else:
                        rings=[]
                        for ring in (x for x in child.iter() if local(x.tag) == "LinearRing"):
                            coord=next((x for x in ring if local(x.tag) == "coordinates"), None)
                            if coord is not None and coord.text:
                                ring=[[float(v) for v in token.split(",")] for token in coord.text.split()]
                                if any(len(v)>2 for v in ring): props["source_kml_altitude_m"]=[v[2] for v in ring if len(v)>2]
                                rings.append([v[:2] for v in ring])
                        if rings: geom = ("Polygon", rings)
            if geom is None: raise GeometryInputError(f"KML Placemark {index} has no supported Point, LineString, or Polygon")
            identity = _property(props, fields.get("id"), required=True, label="id field")
            kind = _property(props, fields.get("feature_type"), required=True, label="feature_type field")
            result.append((identity, kind, geom[0], geom[1], props))
        if not result: raise GeometryInputError("KML contains no supported Placemarks")
        return result, "KML longitude/latitude converted to local ENU metres; source altitude retained as metadata but excluded because KML vertical datum/mode is not established"
    raise GeometryInputError(f"unsupported source format {suffix or '(no extension)'}; supported: .geojson, .json, .csv, .kml, .kmz")


def import_source(path: Path, mapping_path: Path) -> dict[str, Any]:
    """Import GeoJSON/CSV plus an explicit mapping into canonical schema 2.0.0."""
    path, mapping_path = Path(path), Path(mapping_path)
    config = _load_mapping(mapping_path)
    raw = path.read_bytes()
    records, conversion = _raw_features(path, raw, config)
    source_entry = _source_entry(path, raw, config, path.suffix.lower().lstrip("."))
    features = [_feature(identity, kind, geometry_type, coords, props, config, source_entry, conversion=conversion)
                for identity, kind, geometry_type, coords, props in records]
    features.sort(key=lambda f: f["geometry_id"])
    dataset = {
        "schema_version": SCHEMA_VERSION,
        "airport": {**config["airport"], "source_crs": config["source_crs"], "normalization_crs": "LOCAL_ENU",
                    "coordinate_units": "m", "reference_point": config.get("origin")},
        "coordinate_system": {"source_crs": config["source_crs"], "normalization_crs": "LOCAL_ENU",
                              "units": "m", "axes": ["east", "north", "up"],
                              "transformation": conversion,"transform_parameters":{"origin":config.get("origin"),"local_transform":config.get("local_transform"),
                              "source_units":config["source_units"]}, "tolerance_m": float(config.get("tolerance_m", 0.01))},
        "sources": [source_entry], "features": features, "quarantined_features": [],
        "operational_graph": {"nodes": [], "edges": []},
        "operational_relationships": [], "clearance_assessments": [],
        "authority_policy": authority_policy(),
        "dataset_id": config.get("dataset_id", f"{config['airport']['airport_id'].lower()}_{source_entry['source_id']}")
    }
    return dataset


def convert_goal45_dataset(path: Path) -> dict[str, Any]:
    """Migrate the Goal45 active/rejected collections without promoting quarantine."""
    path = Path(path)
    old = _read_json(path)
    if not isinstance(old, dict) or old.get("schema_version") != "1.0" or not isinstance(old.get("features"), list):
        raise GeometryInputError("expected a Goal45 operational geometry schema 1.0 dataset")
    raw_hash = _sha(path.read_bytes())
    repo_root=Path(__file__).resolve().parents[2]
    source_paths={"faa_nasr_2026_10_01":repo_root/"tools/airport_data_ingestion/sources/faa_nasr_auo_2026-10-01.json",
                  "legacy_goal31_geometry":repo_root/"tools/airport_scenario_generation/kauo_goal31.geometry.json"}
    old_sources = {s.get("source_id"): s for s in old.get("sources", [])}
    used_source_ids = sorted({f.get("source_id") for f in old.get("features", []) + old.get("rejected_features", []) if f.get("source_id")})
    sources=[]
    class_for_source={}
    for feature in old.get("features", []) + old.get("rejected_features", []):
        class_for_source.setdefault(feature.get("source_id"), set()).add(feature.get("classification", "UNKNOWN"))
    class_map={"AUTHORITATIVE":"AUTHORITATIVE", "SOURCE_DERIVED":"SOURCE_DERIVED", "APPROXIMATE":"APPROXIMATE", "SYNTHETIC":"SYNTHETIC"}
    for sid in used_source_ids:
        source=old_sources.get(sid, {})
        source_path=source_paths.get(sid)
        if source_path is not None and source_path.is_file():
            source_bytes=source_path.read_bytes(); source_hash=_sha(source_bytes); source_filename=source_path.relative_to(repo_root).as_posix()
        else:
            source_hash=raw_hash; source_filename=path.name
        classes=class_for_source.get(sid, {"UNKNOWN"})
        if classes.issubset({"AUTHORITATIVE", "SOURCE_DERIVED"}):
            authority="AUTHORITATIVE"
        else:
            authority=class_map.get(next(iter(classes)) if len(classes)==1 else "UNKNOWN", "UNKNOWN")
        sources.append({"source_id":sid, "source_type":source.get("data_type", "Goal45 canonical source record"),
                        "source_file":source_filename, "source_url":source.get("url"), "source_sha256":source_hash,
                        "publication_date":source.get("version_date"), "extraction_method":"Goal45 schema 1.0 provenance migration",
                        "authority":authority, "original_publisher":source.get("publisher"),
                        "original_title":source.get("title"), "notes":"Hash identifies the committed source artifact named in source_file; import_metadata separately hashes the Goal45 canonical input."})
    source_map={s["source_id"]:s for s in sources}
    def translate(row: dict[str, Any], *, quarantined: bool) -> dict[str, Any]:
        sid=row.get("source_id")
        cls=row.get("classification", "UNKNOWN")
        authority=class_map.get(cls, "UNKNOWN")
        kind=row.get("feature_type", "other")
        if quarantined and kind=="legacy_node": kind="routing_node"
        if quarantined and kind=="legacy_graph_edge": kind="routing_edge"
        geom=row.get("geometry_type")
        keep={k:v for k,v in row.items() if k not in {"geometry_id","feature_type","geometry_type","coordinates","source_id","source_type","source_date","capture_method","classification","authoritative","accuracy_m","confidence","simulation_approved","notes","coordinate_reference_system"}}
        source=source_map[sid]
        capture=row.get("capture_method", "Goal45 schema migration")
        origin="derived" if authority=="SOURCE_DERIVED" else "direct"
        lineage=[]
        if kind=="runway" and geom=="LineString":
            prefix="runway_"+str(row.get("runway_id", "")).replace("/", "_")+"_threshold_"
            lineage=sorted(f["geometry_id"] for f in old.get("features", []) if f.get("geometry_id", "").startswith(prefix))
        if origin=="derived" and lineage:
            pass
        else:
            origin="direct"; lineage=[]
        prov={"source_id":sid, "original_feature_id":row.get("geometry_id"), "source_file":source["source_file"],
              "source_sha256":source["source_sha256"], "source_url":source.get("source_url"), "publication_date":row.get("source_date") or source.get("publication_date"),
              "extraction_method":capture, "transformation":row.get("coordinate_reference_system", "Goal45 local ENU; no coordinate change"),
              "authority":authority, "geometry_origin":origin, "accuracy_m":row.get("accuracy_m"),
              "simulation_approved":bool(row.get("simulation_approved", False)) and not quarantined,
              "confidence":row.get("confidence"), "notes":row.get("notes", "")}
        if lineage: prov["derived_from"]=lineage
        return {"geometry_id":row.get("geometry_id"), "feature_type":kind, "geometry_type":geom,
                "coordinates":row.get("coordinates"), "properties":keep, "provenance":prov}
    active=[translate(f, quarantined=False) for f in old.get("features", [])]
    quarantine=[translate(f, quarantined=True) for f in old.get("rejected_features", [])]
    anchors=_read_json(Path(__file__).resolve().parents[1]/"airport_scenario_generation"/"kauo.anchors.json")
    origin=anchors["origin"]
    dataset={"schema_version":SCHEMA_VERSION,"dataset_id":"kauo_goal46_from_goal45",
             "import_metadata":{"source_file":path.name,"source_sha256":raw_hash,"source_schema_version":"1.0"},
             "airport":{"airport_id":old.get("airport_id","KAUO"),"source_crs":"EPSG:4979 (source) and LOCAL_ENU (Goal45 canonical)",
                        "normalization_crs":"LOCAL_ENU","coordinate_units":"m",
                        "reference_point":{"latitude_degrees":origin["latitude_degrees"],"longitude_degrees":origin["longitude_degrees"],
                                           "ellipsoid_height_m":origin["ellipsoid_height_m"],"coordinates_enu_m":[0.0,0.0]},
                        "elevation_ft_msl":origin.get("field_elevation_ft_msl")},
             "coordinate_system":{"source_crs":"EPSG:4979 to Goal45 LOCAL_ENU","normalization_crs":"LOCAL_ENU","units":"m",
                                  "axes":["east","north","up"],"transformation":"Inherited Goal45 WGS-84 ECEF to local tangent-plane ENU; existing active coordinates retained without retransform.","tolerance_m":0.001},
             "sources":sources,"features":active,"quarantined_features":quarantine,
             "operational_graph":{"nodes":[],"edges":[],"status":"unavailable"},
             "operational_relationships":old.get("operational_relationships",[]),
             "clearance_assessments":old.get("clearance_assessments",[]),
             "authority_policy":authority_policy(),"required_runway_ids":old.get("required_runway_ids",[])}
    return dataset


def authority_policy() -> dict[str, Any]:
    return {
        "policy_version": "1.0",
        "production_routing": {"allowed_authorities": sorted(PRODUCTION_AUTHORITIES), "requires_complete_provenance": True,
                               "derived_geometry_requires_lineage": True, "inferred_geometry_allowed": False},
        "visualization": {"allowed_authorities": sorted(AUTHORITIES)},
        "testing": {"synthetic_allowed_only_in_test_fixture_scope": True},
        "safety_claims": {"approximate_or_synthetic_requires_explicit_qualification": True},
    }


def validate_dataset(dataset: Any) -> list[dict[str, Any]]:
    findings: list[dict[str, Any]] = []
    def add(code: str, path: str, message: str, severity: str = "ERROR") -> None:
        findings.append({"code": code, "path": path, "severity": severity, "message": message})
    if not isinstance(dataset, dict):
        return [{"code": "DATASET_TYPE", "path": "$", "severity": "ERROR", "message": "dataset must be an object"}]
    if dataset.get("schema_version") != SCHEMA_VERSION:
        add("SCHEMA_VERSION", "schema_version", f"expected {SCHEMA_VERSION}")
    airport = dataset.get("airport")
    if not isinstance(airport, dict) or not str(airport.get("airport_id", "")).strip():
        add("AIRPORT_ID", "airport.airport_id", "airport identifier is required")
    cs = dataset.get("coordinate_system")
    if not isinstance(cs, dict) or cs.get("normalization_crs") != "LOCAL_ENU" or cs.get("units") != "m" or cs.get("axes") != ["east", "north", "up"]:
        add("COORDINATE_SYSTEM", "coordinate_system", "canonical coordinates must declare LOCAL_ENU metres and east/north/up axes")
    if dataset.get("authority_policy")!=authority_policy():
        add("AUTHORITY_POLICY","authority_policy","authority policy must match the non-weakenable Goal46 production and test policy")
    sources = dataset.get("sources")
    if not isinstance(sources, list):
        add("SOURCES", "sources", "sources must be an array"); sources = []
    source_map: dict[str, dict[str, Any]] = {}
    for i, source in enumerate(sources):
        p = f"sources[{i}]"
        if not isinstance(source, dict):
            add("SOURCE_TYPE", p, "source entry must be an object"); continue
        sid = source.get("source_id")
        if not sid or sid in source_map: add("SOURCE_ID", p, "source_id must be nonempty and unique")
        else: source_map[sid] = source
        for key in ("source_type", "source_file", "source_sha256", "extraction_method", "authority"):
            if not source.get(key): add("SOURCE_FIELD", f"{p}.{key}", f"{key} is required")
        if source.get("authority") not in AUTHORITIES: add("AUTHORITY", f"{p}.authority", "authority value is unsupported")
        if source.get("source_sha256") and not re.fullmatch(r"[0-9a-f]{64}", str(source["source_sha256"])):
            add("SOURCE_HASH", f"{p}.source_sha256", "source_sha256 must be a lowercase SHA-256 hex digest")
    features = dataset.get("features")
    if not isinstance(features, list):
        add("FEATURES", "features", "features must be an array"); features = []
    quarantined=dataset.get("quarantined_features", [])
    if not isinstance(quarantined, list):
        add("QUARANTINE", "quarantined_features", "quarantined_features must be an array"); quarantined=[]
    all_features=[("features", f) for f in features]+[("quarantined_features", f) for f in quarantined]
    seen: set[str] = set()
    feature_ids: set[str] = set()
    for i, (collection, feature) in enumerate(all_features):
        p = f"{collection}[{i}]"
        if not isinstance(feature, dict): add("FEATURE_TYPE", p, "feature must be an object"); continue
        ident = feature.get("geometry_id")
        if not ident or ident in seen: add("FEATURE_ID", f"{p}.geometry_id", "geometry_id must be nonempty and unique")
        else: seen.add(ident); feature_ids.add(str(ident))
        kind, geom = feature.get("feature_type"), feature.get("geometry_type")
        if kind not in FEATURE_TYPES: add("FEATURE_KIND", f"{p}.feature_type", f"unsupported feature type {kind!r}")
        if geom not in GEOMETRIES: add("GEOMETRY_TYPE", f"{p}.geometry_type", f"unsupported geometry type {geom!r}")
        elif kind in FEATURE_TYPES and geom not in _geometry_type_for_feature_type(kind) and kind!="other":
            add("FEATURE_GEOMETRY_MISMATCH",f"{p}.geometry_type",f"{kind} cannot use {geom} geometry")
        props=feature.get("properties",{})
        if not isinstance(props,dict): add("FEATURE_PROPERTIES",f"{p}.properties","properties must be an object"); props={}
        for key in ("width_m","clearance_m"):
            value=props.get(key)
            if value is not None and (not _finite_number(value) or float(value)<0 or (key=="width_m" and float(value)==0)):
                add("FEATURE_MEASURE",f"{p}.properties.{key}",f"{key} must be null or a finite positive/nonnegative metre value")
        for key in ("directed","permitted"):
            if key in props and not isinstance(props[key],bool): add("FEATURE_BOOLEAN",f"{p}.properties.{key}",f"{key} must be an explicit boolean")
        prov = feature.get("provenance")
        if not isinstance(prov, dict):
            add("PROVENANCE_MISSING", f"{p}.provenance", "feature provenance object is required"); continue
        for key in REQUIRED_PROVENANCE:
            if prov.get(key) in (None, ""): add("PROVENANCE_FIELD", f"{p}.provenance.{key}", f"{key} is required")
        if prov.get("source_id") not in source_map: add("PROVENANCE_SOURCE", f"{p}.provenance.source_id", "source_id must resolve to a source entry")
        elif prov.get("source_sha256") != source_map[prov.get("source_id")].get("source_sha256"):
            add("PROVENANCE_HASH", f"{p}.provenance.source_sha256", "feature source hash must match its source registry entry")
        if prov.get("authority") not in AUTHORITIES: add("PROVENANCE_AUTHORITY", f"{p}.provenance.authority", "unsupported authority")
        elif prov.get("source_id") in source_map:
            source_authority=source_map[prov.get("source_id")].get("authority")
            allowed={source_authority}
            if source_authority in {"AUTHORITATIVE","SURVEYED","OPERATOR_PROVIDED"}: allowed.add("SOURCE_DERIVED")
            if source_authority=="AUTHORITATIVE": allowed.add("APPROXIMATE")
            if prov.get("authority") not in allowed:
                add("AUTHORITY_MISMATCH",f"{p}.provenance.authority",f"feature authority {prov.get('authority')} conflicts with registered source authority {source_authority}")
        if prov.get("authority") in {"APPROXIMATE", "SYNTHETIC", "UNKNOWN"} and prov.get("simulation_approved") is True:
            add("FORBIDDEN_SIMULATION_APPROVAL", f"{p}.provenance.simulation_approved", "approximate, synthetic, or unknown geometry cannot be approved for production routing")
        if not isinstance(prov.get("simulation_approved"),bool):
            add("SIMULATION_APPROVAL_TYPE",f"{p}.provenance.simulation_approved","simulation_approved must be an explicit boolean")
        if prov.get("geometry_origin") not in {"direct","derived","inferred","approximate"}:
            add("GEOMETRY_ORIGIN",f"{p}.provenance.geometry_origin","geometry_origin must be direct, derived, inferred, or approximate")
        accuracy = prov.get("accuracy_m")
        if accuracy is not None and (not _finite_number(accuracy) or float(accuracy) < 0):
            add("ACCURACY", f"{p}.provenance.accuracy_m", "accuracy_m must be null or a finite nonnegative number")
        confidence=prov.get("confidence")
        if confidence is not None and (not _finite_number(confidence) or not 0<=float(confidence)<=1):
            add("CONFIDENCE",f"{p}.provenance.confidence","confidence must be null or a finite number from 0 to 1")
        if prov.get("geometry_origin") in {"derived", "inferred"} and not prov.get("derived_from"):
            add("LINEAGE_MISSING", f"{p}.provenance.derived_from", "derived or inferred geometry requires source-feature lineage")
        if prov.get("geometry_origin") in {"derived", "inferred"} and isinstance(prov.get("derived_from"), list):
            for parent in prov["derived_from"]:
                if parent not in feature_ids and parent not in {f.get("geometry_id") for _, f in all_features if isinstance(f, dict)}:
                    add("LINEAGE_SOURCE_MISSING", f"{p}.provenance.derived_from", f"lineage feature {parent!r} is absent")
        if prov.get("geometry_origin") == "inferred":
            add("INFERRED_PRODUCTION_GEOMETRY", p, "inferred geometry is not admissible for production routing")
        if isinstance(feature.get("coordinates"), list):
            points = list(_positions(feature["coordinates"], geom)) if geom in GEOMETRIES else []
            for n, point in enumerate(points):
                if not isinstance(point, list) or len(point) not in (2, 3) or not all(_finite_number(v) for v in point):
                    add("COORDINATE_INVALID", f"{p}.coordinates[{n}]", "coordinates must be finite ENU positions with two or three values")
        else: add("COORDINATES", f"{p}.coordinates", "coordinates must be an array")
    active_feature_ids={str(f.get("geometry_id")) for f in features if isinstance(f,dict) and f.get("geometry_id")}
    quarantined_ids={str(f.get("geometry_id")) for f in quarantined if isinstance(f,dict) and f.get("geometry_id")}
    for collection,rows,allowed_ids in (("features",features,active_feature_ids),("quarantined_features",quarantined,quarantined_ids)):
        for i,feature in enumerate(rows):
            if not isinstance(feature,dict): continue
            parents=feature.get("provenance",{}).get("derived_from",[])
            if parents and not set(parents).issubset(allowed_ids):
                add("LINEAGE_CROSSES_QUARANTINE",f"{collection}[{i}].provenance.derived_from","lineage cannot promote a feature across the active/quarantined boundary")
    relations=dataset.get("operational_relationships",[])
    if not isinstance(relations,list): add("RELATIONSHIPS","operational_relationships","operational_relationships must be an array"); relations=[]
    for i,relation in enumerate(relations):
        p=f"operational_relationships[{i}]"
        if not isinstance(relation,dict): add("RELATIONSHIP_TYPE",p,"relationship must be an object"); continue
        if relation.get("source_id") not in source_map: add("RELATIONSHIP_SOURCE",p,"relationship source_id must resolve to a source")
        refs=relation.get("source_feature_ids",[])
        if not refs or not set(refs).issubset(active_feature_ids): add("RELATIONSHIP_LINEAGE",p,"relationship must cite active source feature IDs")
        if relation.get("inferred") is True: add("RELATIONSHIP_INFERRED",p,"inferred operational relationships cannot satisfy production readiness")
    assessments=dataset.get("clearance_assessments",[])
    if not isinstance(assessments,list): add("CLEARANCES","clearance_assessments","clearance_assessments must be an array"); assessments=[]
    for i,assessment in enumerate(assessments):
        p=f"clearance_assessments[{i}]"
        if not isinstance(assessment,dict): add("CLEARANCE_TYPE",p,"assessment must be an object"); continue
        if assessment.get("source_id") not in source_map: add("CLEARANCE_SOURCE",p,"assessment source_id must resolve to a source")
        refs=assessment.get("feature_ids",[])
        if not refs or not set(refs).issubset(active_feature_ids): add("CLEARANCE_LINEAGE",p,"clearance assessment must cite active source feature IDs")
        for key in ("required_clearance_m","measured_clearance_m"):
            value=assessment.get(key)
            if value is not None and (not _finite_number(value) or float(value)<0): add("CLEARANCE_VALUE",f"{p}.{key}","clearance values must be null or finite nonnegative metres")
        if not isinstance(assessment.get("simulation_approved"),bool): add("CLEARANCE_APPROVAL",f"{p}.simulation_approved","clearance approval must be an explicit boolean")
    graph=dataset.get("operational_graph",{})
    if not isinstance(graph,dict):
        add("GRAPH_TYPE","operational_graph","operational_graph must be an object"); graph={}
    graph_nodes=graph.get("nodes",[]); graph_edges=graph.get("edges",[])
    if not isinstance(graph_nodes,list): add("GRAPH_NODES","operational_graph.nodes","graph nodes must be an array"); graph_nodes=[]
    if not isinstance(graph_edges,list): add("GRAPH_EDGES","operational_graph.edges","graph edges must be an array"); graph_edges=[]
    graph_node_ids=set()
    for i,node in enumerate(graph_nodes):
        p=f"operational_graph.nodes[{i}]"
        if not isinstance(node,dict): add("GRAPH_NODE_TYPE",p,"graph node must be an object"); continue
        node_id=node.get("node_id")
        if not node_id or node_id in graph_node_ids: add("GRAPH_NODE_ID",p,"graph node ID must be nonempty and unique")
        graph_node_ids.add(node_id)
        if not isinstance(node.get("coordinates"),list) or len(node["coordinates"]) not in (2,3) or not all(_finite_number(v) for v in node["coordinates"]): add("GRAPH_NODE_COORDINATES",p,"graph node requires finite ENU coordinates")
        lineage=node.get("source_feature_ids")
        if not lineage or not set(lineage).issubset(active_feature_ids): add("GRAPH_NODE_LINEAGE",p,"graph node must cite active source geometry")
        provenance=node.get("provenance",{})
        if provenance.get("geometry_origin")=="derived" and not provenance.get("derived_from"): add("GRAPH_NODE_PROVENANCE",p,"derived graph node requires input-feature lineage")
        if provenance.get("authority") not in AUTHORITIES: add("GRAPH_NODE_AUTHORITY",p,"graph node provenance authority is missing or unsupported")
        if not isinstance(provenance.get("simulation_approved"),bool): add("GRAPH_NODE_APPROVAL",p,"graph node simulation approval must be an explicit boolean")
        if not provenance.get("source_ids") or not set(provenance.get("source_ids",[])).issubset(source_map): add("GRAPH_NODE_SOURCES",p,"graph node must cite registered sources")
        if not provenance.get("source_hashes") or not all(re.fullmatch(r"[0-9a-f]{64}",str(h)) for h in provenance.get("source_hashes",[])): add("GRAPH_NODE_HASHES",p,"graph node must retain source hashes")
    graph_edge_ids=set()
    for i,edge in enumerate(graph_edges):
        p=f"operational_graph.edges[{i}]"
        if not isinstance(edge,dict): add("GRAPH_EDGE_TYPE",p,"graph edge must be an object"); continue
        edge_id=edge.get("edge_id")
        if not edge_id or edge_id in graph_edge_ids: add("GRAPH_EDGE_ID",p,"graph edge ID must be nonempty and unique")
        graph_edge_ids.add(edge_id)
        if edge.get("from_node") not in graph_node_ids or edge.get("to_node") not in graph_node_ids: add("GRAPH_EDGE_ENDPOINT",p,"graph edge endpoints must resolve to graph nodes")
        lineage=edge.get("source_feature_ids")
        if not lineage or not set(lineage).issubset(active_feature_ids): add("GRAPH_EDGE_LINEAGE",p,"graph edge must cite active source geometry")
        provenance=edge.get("provenance",{})
        if provenance.get("source_id") not in source_map: add("GRAPH_EDGE_SOURCE",p,"graph edge provenance must resolve to a registered source")
        elif provenance.get("source_sha256")!=source_map[provenance.get("source_id")].get("source_sha256"): add("GRAPH_EDGE_HASH",p,"graph edge source hash must match the registry")
    return sorted(findings, key=lambda f: (f["path"], f["code"], f["message"]))


def _positions(coords: Any, geometry_type: str):
    if geometry_type == "Point": return [coords]
    if geometry_type == "MultiPoint": return coords
    if geometry_type == "LineString": return coords
    if geometry_type == "MultiLineString": return [p for line in coords for p in line]
    if geometry_type == "Polygon": return [p for ring in coords for p in ring]
    if geometry_type == "MultiPolygon": return [p for poly in coords for ring in poly for p in ring]
    return []


def _distance(a: list[float], b: list[float]) -> float:
    return math.hypot(a[0]-b[0], a[1]-b[1])


def _segment_intersection(a: list[float], b: list[float], c: list[float], d: list[float]) -> list[float] | None:
    """Return a 2D segment intersection point, including shared endpoints."""
    rx,ry=b[0]-a[0],b[1]-a[1]; sx,sy=d[0]-c[0],d[1]-c[1]
    denominator=rx*sy-ry*sx
    if abs(denominator)<1e-12: return None
    qpx,qpy=c[0]-a[0],c[1]-a[1]
    t=(qpx*sy-qpy*sx)/denominator; u=(qpx*ry-qpy*rx)/denominator
    if -1e-9<=t<=1+1e-9 and -1e-9<=u<=1+1e-9:
        return [round(a[0]+t*rx,6),round(a[1]+t*ry,6)]
    return None


def _line_intersections(first: list[list[float]], second: list[list[float]]) -> list[list[float]]:
    found=[]
    for a,b in zip(first,first[1:]):
        for c,d in zip(second,second[1:]):
            point=_segment_intersection(a,b,c,d)
            if point is not None and point not in found: found.append(point)
    return found


def point_segment_distance(point: tuple[float,float], start: tuple[float,float], end: tuple[float,float]) -> float:
    dx,dy=end[0]-start[0],end[1]-start[1]
    denom=dx*dx+dy*dy
    t=0.0 if denom==0 else max(0.0,min(1.0,((point[0]-start[0])*dx+(point[1]-start[1])*dy)/denom))
    return math.hypot(point[0]-(start[0]+t*dx),point[1]-(start[1]+t*dy))


def build_graph(dataset: dict[str, Any], snap_tolerance_m: float = 0.25) -> dict[str, Any]:
    """Build graph edges only from supplied routable linework; snap endpoints only."""
    if not _finite_number(snap_tolerance_m) or snap_tolerance_m < 0:
        raise GeometryInputError("snap tolerance must be a finite nonnegative number of metres")
    line_kinds = {"taxiway_centerline", "service_road", "gse_access_link", "routing_edge"}
    parts=[]
    for feature in dataset.get("features", []):
        if feature.get("feature_type") not in line_kinds: continue
        if feature.get("geometry_type")=="LineString": parts.append((feature,0,feature["coordinates"]))
        elif feature.get("geometry_type")=="MultiLineString": parts.extend((feature,index,line) for index,line in enumerate(feature["coordinates"]))
    endpoints = sorted([(round(coords[0][0], 6), round(coords[0][1], 6), f["geometry_id"], part, 0) for f,part,coords in parts] +
                       [(round(coords[-1][0], 6), round(coords[-1][1], 6), f["geometry_id"], part, 1) for f,part,coords in parts])
    # Deterministic union of supplied endpoints. This never creates a connecting edge.
    groups: list[list[tuple[float, float, str, int, int]]] = []
    for endpoint in endpoints:
        match = next((group for group in groups if all(math.hypot(endpoint[0]-p[0], endpoint[1]-p[1]) <= snap_tolerance_m for p in group)), None)
        if match is not None: match.append(endpoint)
        else: groups.append([endpoint])
    node_for: dict[tuple[str, int, int], str] = {}
    nodes = []
    for index, group in enumerate(sorted(groups, key=lambda g: (min(p[0] for p in g), min(p[1] for p in g), sorted((p[2], p[3],p[4]) for p in g)))):
        node_id = f"n{index+1:04d}"
        x = round(sum(p[0] for p in group) / len(group), 6); y = round(sum(p[1] for p in group) / len(group), 6)
        for p in group: node_for[(p[2], p[3],p[4])] = node_id
        parents=sorted({p[2] for p in group})
        source_features=[f for f,_,_ in parts if f["geometry_id"] in parents]
        authorities={f.get("provenance",{}).get("authority") for f in source_features}
        if "UNKNOWN" in authorities: node_authority="UNKNOWN"
        elif "APPROXIMATE" in authorities: node_authority="APPROXIMATE"
        elif "SYNTHETIC" in authorities: node_authority="SYNTHETIC"
        elif len(authorities)==1: node_authority=next(iter(authorities))
        else: node_authority="SOURCE_DERIVED"
        node_approved=all(f.get("provenance",{}).get("simulation_approved") is True and f.get("provenance",{}).get("authority") in PRODUCTION_AUTHORITIES for f in source_features)
        nodes.append({"node_id": node_id, "coordinates": [x, y], "source_feature_ids": parents,
                      "snap_tolerance_m": round(float(snap_tolerance_m), 6),
                      "provenance":{"geometry_origin":"derived","derived_from":parents,"authority":node_authority,
                                    "simulation_approved":node_approved,"source_ids":sorted({f.get("provenance",{}).get("source_id") for f in source_features}),
                                    "source_hashes":sorted({f.get("provenance",{}).get("source_sha256") for f in source_features}),
                                    "transformation":"Deterministic endpoint snap of supplied linework only; no connecting infrastructure added."}})
    edges = []
    for feature,part_index,coordinates in sorted(parts, key=lambda item:(item[0]["geometry_id"],item[1])):
        start = node_for[(feature["geometry_id"], part_index, 0)]; end = node_for[(feature["geometry_id"], part_index, 1)]
        props = feature.get("properties", {})
        length=sum(_distance(a,b) for a,b in zip(coordinates,coordinates[1:]))
        edges.append({"edge_id": f"edge:{feature['geometry_id']}:part:{part_index}", "from_node": start, "to_node": end,
                      "directed": props.get("directed"), "geometry_id": feature["geometry_id"],
                      "source_feature_ids": [feature["geometry_id"]],
                      "geometry_coordinates": coordinates,"length_m":round(length,6),
                      "permissions": props.get("permissions", []),
                      "runway_crossing_permission": props.get("runway_crossing_permission", "unknown"),
                      "width_m": props.get("width_m"), "clearance_m": props.get("clearance_m"),
                      "provenance": feature["provenance"]})
    dataset["operational_graph"] = {"nodes": nodes, "edges": sorted(edges, key=lambda e: e["edge_id"]),
                                    "construction": {"method": "supplied line endpoints with deterministic endpoint snapping",
                                                     "snap_tolerance_m": round(float(snap_tolerance_m), 6),
                                                     "no_gap_bridging": True}}
    return dataset


def graph_diagnostics(dataset: dict[str, Any]) -> list[dict[str, Any]]:
    graph = dataset.get("operational_graph", {})
    nodes = {n.get("node_id"): n for n in graph.get("nodes", [])}
    edges = graph.get("edges", [])
    findings = []
    adjacency = {node_id: set() for node_id in nodes}
    ids = set()
    for edge in edges:
        eid = edge.get("edge_id")
        if eid in ids: findings.append({"code": "DUPLICATE_EDGE", "feature_id": eid, "message": "duplicate graph edge id"})
        ids.add(eid)
        start, end = edge.get("from_node"), edge.get("to_node")
        if start not in nodes or end not in nodes:
            findings.append({"code": "MISSING_GRAPH_NODE", "feature_id": eid, "message": "edge references a missing graph node"}); continue
        if start == end:
            findings.append({"code": "ZERO_LENGTH_EDGE", "feature_id": eid, "message": "edge endpoints are the same graph node"})
        if edge.get("directed") in (True, False):
            adjacency[start].add(end)
            if edge.get("directed") is False: adjacency[end].add(start)
        if not edge.get("source_feature_ids"):
            findings.append({"code": "EDGE_LINEAGE_MISSING", "feature_id": eid, "message": "graph edge has no input-feature lineage"})
        if edge.get("directed") not in (True, False):
            findings.append({"code":"EDGE_DIRECTION_UNKNOWN","feature_id":eid,"message":"directionality is missing; direction cannot be assumed"})
        if not isinstance(edge.get("permissions"),list) or not edge.get("permissions"):
            findings.append({"code":"EDGE_PERMISSIONS_UNKNOWN","feature_id":eid,"message":"vehicle permissions are missing; access cannot be assumed"})
    components = []
    unseen = set(nodes)
    while unseen:
        seed = min(unseen); stack = [seed]; found = set()
        while stack:
            current = stack.pop()
            if current in found: continue
            found.add(current); stack.extend(sorted(adjacency[current] - found, reverse=True))
        unseen -= found; components.append(sorted(found))
    if len(components) > 1:
        findings.append({"code": "DISCONNECTED_COMPONENTS", "components": components, "message": f"routing graph has {len(components)} disconnected components"})
    used = {n for e in edges for n in (e.get("from_node"), e.get("to_node"))}
    for node_id in sorted(set(nodes) - used):
        findings.append({"code": "DANGLING_NODE", "feature_id": node_id, "message": "graph node is not connected to an edge"})
    degree={node_id:sum(1 for e in edges if node_id in (e.get("from_node"),e.get("to_node"))) for node_id in nodes}
    for node_id in sorted(n for n,d in degree.items() if d==1):
        findings.append({"code":"DANGLING_ENDPOINT","severity":"WARNING","feature_id":node_id,"message":"network endpoint has only one incident route edge; confirm it is an intentional terminus"})
    line_kinds={"taxiway_centerline","service_road","gse_access_link","routing_edge"}
    line_features=[]
    for feature in dataset.get("features",[]):
        if feature.get("feature_type") not in line_kinds: continue
        if feature.get("geometry_type")=="LineString": parts=[feature.get("coordinates",[])]
        elif feature.get("geometry_type")=="MultiLineString": parts=feature.get("coordinates",[])
        else: continue
        line_features.append((feature,parts))
        for part_index,line in enumerate(parts):
            for segment_index,(a,b) in enumerate(zip(line,line[1:])):
                if _distance(a,b)<=1e-9:
                    findings.append({"code":"ZERO_LENGTH_SEGMENT","feature_id":feature.get("geometry_id"),"message":f"line part {part_index} segment {segment_index} has zero length"})
            for i in range(len(line)-1):
                for j in range(i+2,len(line)-1):
                    if i==0 and j==len(line)-2 and line[0]==line[-1]: continue
                    if _segment_intersection(line[i],line[i+1],line[j],line[j+1]) is not None:
                        findings.append({"code":"SELF_INTERSECTION","feature_id":feature.get("geometry_id"),"message":f"line part {part_index} crosses itself between segments {i} and {j}"})
    tolerance=float(dataset.get("operational_graph",{}).get("construction",{}).get("snap_tolerance_m",0.0))
    seen_geometry={}
    for feature,parts in line_features:
        normalized=tuple(tuple(tuple(round(float(v),6) for v in point[:2]) for point in part) for part in parts)
        reversed_geometry=tuple(tuple(reversed(part)) for part in reversed(normalized))
        key=min(normalized,reversed_geometry)
        if key in seen_geometry:
            findings.append({"code":"DUPLICATE_GEOMETRY","feature_id":feature.get("geometry_id"),"message":f"route geometry duplicates {seen_geometry[key]}"})
        else: seen_geometry[key]=feature.get("geometry_id")
    for i,(left,left_parts) in enumerate(line_features):
        for right,right_parts in line_features[i+1:]:
            for lp in left_parts:
                for rp in right_parts:
                    for point in _line_intersections(lp,rp):
                        left_endpoint=any(_distance(point,p)<=tolerance for p in (lp[0],lp[-1]))
                        right_endpoint=any(_distance(point,p)<=tolerance for p in (rp[0],rp[-1]))
                        if not (left_endpoint and right_endpoint):
                            findings.append({"code":"AMBIGUOUS_INTERSECTION","feature_id":f"{left.get('geometry_id')}|{right.get('geometry_id')}",
                                             "coordinates":point,"message":"supplied route lines intersect away from a shared endpoint; no graph connection was inferred"})
            left_ends=[p for part in left_parts for p in (part[0],part[-1])]
            right_ends=[p for part in right_parts for p in (part[0],part[-1])]
            for a in left_ends:
                for b in right_ends:
                    gap=_distance(a,b)
                    if tolerance<gap<=1.0:
                        findings.append({"code":"SUSPICIOUS_NEAR_MISS","severity":"WARNING","feature_id":f"{left.get('geometry_id')}|{right.get('geometry_id')}",
                                         "gap_m":round(gap,6),"message":"line endpoints are close but outside the configured snap tolerance; no connection was added"})
    return sorted(findings, key=lambda f: (f.get("code", ""), f.get("feature_id", "")))


def _reachable(graph: dict[str, Any], start: str, target: str, permission: str = "aircraft") -> bool:
    return _find_path(graph, start, target, permission) is not None


def _find_path(graph: dict[str, Any], start: str, target: str, permission: str = "aircraft") -> list[str] | None:
    if start == target: return [start]
    adjacency: dict[str, set[str]] = {}
    for edge in graph.get("edges", []):
        perms = edge.get("permissions", [])
        if permission not in perms or edge.get("directed") not in (True, False): continue
        a, b = edge.get("from_node"), edge.get("to_node")
        adjacency.setdefault(a, set()).add(b)
        if edge.get("directed") is False: adjacency.setdefault(b, set()).add(a)
    todo = [start]; previous: dict[str, str | None] = {start: None}
    while todo:
        current = todo.pop()
        if current == target:
            path=[]; node: str | None = target
            while node is not None: path.append(node); node=previous[node]
            return list(reversed(path))
        for neighbor in sorted(adjacency.get(current, set()), reverse=True):
            if neighbor not in previous:
                previous[neighbor]=current; todo.append(neighbor)
    return None


def audit_clearances(rows: list[dict[str, Any]], *, allow_unapproved: bool = False) -> list[dict[str, Any]]:
    results = []
    for row in sorted(rows, key=lambda item: item.get("assessment_id", "")):
        required, measured = row.get("required_clearance_m"), row.get("measured_clearance_m")
        if (not _finite_number(required) or not _finite_number(measured) or not row.get("source_id")
                or not row.get("assessment_method") or (not allow_unapproved and row.get("simulation_approved") is not True)):
            status = "UNKNOWN"
        else: status = "PASS" if float(measured) >= float(required) else "FAIL"
        results.append({"assessment_id": row.get("assessment_id", ""), "status": status,
                        "required_clearance_m": required, "measured_clearance_m": measured,
                        "source_id": row.get("source_id"), "feature_ids": sorted(row.get("feature_ids", []))})
    return results


def readiness(dataset: dict[str, Any], *, profile: str = "production") -> dict[str, Any]:
    if profile not in {"production", "test_fixture"}:
        raise GeometryInputError("readiness profile must be production or test_fixture")
    features = dataset.get("features", [])
    by_type: dict[str, list[dict[str, Any]]] = {}
    for feature in features: by_type.setdefault(feature.get("feature_type"), []).append(feature)
    valid_findings = validate_dataset(dataset)
    graph_issues = graph_diagnostics(dataset)
    blocking_graph_issues=[issue for issue in graph_issues if issue.get("severity")!="WARNING"]
    graph = dataset.get("operational_graph", {})
    authority_allowed = (PRODUCTION_AUTHORITIES if profile == "production" else AUTHORITIES - {"UNKNOWN"})
    active_authorities = {f.get("provenance", {}).get("authority") for f in features}
    graph_elements=dataset.get("operational_graph",{}).get("nodes",[])+dataset.get("operational_graph",{}).get("edges",[])
    graph_authorities={n.get("provenance",{}).get("authority") for n in graph_elements}
    authority_ok = bool(features) and active_authorities.issubset(authority_allowed) and (
        graph_authorities.issubset(authority_allowed) and
        (profile == "test_fixture" or (all(f.get("provenance", {}).get("simulation_approved") is True for f in features)
         and all(e.get("provenance",{}).get("simulation_approved") is True for e in graph_elements))))
    source_registry={s.get("source_id"):s for s in dataset.get("sources",[])}
    active_feature_ids={f.get("geometry_id") for f in features}
    clearance_inputs=[]
    for source_row in dataset.get("clearance_assessments",[]):
        row=dict(source_row)
        source=source_registry.get(row.get("source_id"),{})
        valid_refs=bool(row.get("feature_ids")) and set(row.get("feature_ids",[])).issubset(active_feature_ids)
        source_allowed=profile=="test_fixture" or source.get("authority") in PRODUCTION_AUTHORITIES
        if not valid_refs or not source_allowed: row["simulation_approved"]=False
        clearance_inputs.append(row)
    clearance_rows = audit_clearances(clearance_inputs, allow_unapproved=profile=="test_fixture")
    clearance_statuses = {r["status"] for r in clearance_rows}
    crossings = by_type.get("runway_crossing", [])
    taxiways = by_type.get("taxiway_centerline", [])
    stands = by_type.get("aircraft_stand", [])
    depots = by_type.get("gse_depot", []) + by_type.get("gse_staging_area", [])
    roads = by_type.get("service_road", [])
    nodes = {n.get("node_id") for n in graph.get("nodes", [])}
    node_map={n.get("node_id"):n for n in graph.get("nodes", [])}
    relations = dataset.get("operational_relationships", [])
    source_ids={s.get("source_id") for s in dataset.get("sources",[])}
    crossing_intersections=[]
    runway_lines=[f for f in by_type.get("runway",[]) if f.get("geometry_type")=="LineString"]
    for taxiway in taxiways:
        lines=[taxiway.get("coordinates",[])] if taxiway.get("geometry_type")=="LineString" else taxiway.get("coordinates",[]) if taxiway.get("geometry_type")=="MultiLineString" else []
        for runway in runway_lines:
            for line in lines:
                for point in _line_intersections(line,runway.get("coordinates",[])):
                    matching=[]
                    for crossing in crossings:
                        coords=crossing.get("coordinates",[])
                        if crossing.get("geometry_type")=="Point" and _distance(point,coords)<=1.0: matching.append(crossing)
                        elif crossing.get("geometry_type")=="LineString" and any(point_segment_distance(tuple(point),tuple(a),tuple(b))<=1.0 for a,b in zip(coords,coords[1:])): matching.append(crossing)
                    line_props=taxiway.get("properties",{})
                    permitted=(line_props.get("runway_crossing_permission")=="permitted"
                               and line_props.get("crossing_permission_source_id") in source_ids
                               and any(c.get("properties",{}).get("permitted") is True
                                       and c.get("properties",{}).get("permission_source_id") in source_ids for c in matching))
                    crossing_intersections.append({"taxiway_feature_id":taxiway.get("geometry_id"),"runway_feature_id":runway.get("geometry_id"),
                                                   "coordinates":point,"crossing_feature_ids":[c.get("geometry_id") for c in matching],
                                                   "status":"PASS" if permitted else "FAIL",
                                                   "message":"Crossing has source-backed permission." if permitted else "Crossing lacks explicit source-backed permission geometry."})
    if crossing_intersections:
        crossing_status="PASS" if all(row["status"]=="PASS" for row in crossing_intersections) else "BLOCKED"
    elif graph.get("edges"):
        crossing_status="NOT_APPLICABLE"
    else:
        crossing_status="BLOCKED"
    runway_relations=[r for r in relations if r.get("relation_type") in {"runway_entry", "runway_exit", "runway_connection"}
                      and r.get("source_id") in source_ids]
    exit_nodes=sorted({r.get("from_node") for r in runway_relations if r.get("relation_type") in {"runway_exit","runway_connection"} and r.get("from_node") in nodes})
    entry_nodes=sorted({r.get("to_node") for r in runway_relations if r.get("relation_type") in {"runway_entry","runway_connection"} and r.get("to_node") in nodes})
    runway_nodes=sorted(set(exit_nodes+entry_nodes))
    def attached(feature: dict[str, Any]) -> bool:
        node_id=feature.get("properties", {}).get("network_node_id")
        if node_id not in node_map: return False
        coords=feature.get("coordinates")
        node_coords=node_map[node_id].get("coordinates")
        if not isinstance(coords,list) or not isinstance(node_coords,list) or _distance(coords,node_coords)>float(dataset.get("coordinate_system",{}).get("tolerance_m",0.01))+0.25:
            return False
        return True
    stand_routes=[]
    for stand in sorted(stands,key=lambda x:x.get("geometry_id","")):
        start=stand.get("properties",{}).get("network_node_id")
        departure=next((_find_path(graph,start,target,"aircraft") for target in entry_nodes if start in nodes and _find_path(graph,start,target,"aircraft")),None)
        arrival=next((_find_path(graph,target,start,"aircraft") for target in exit_nodes if start in nodes and _find_path(graph,target,start,"aircraft")),None)
        status="PASS" if attached(stand) and departure and arrival else "FAIL"
        stand_routes.append({"feature_id":stand.get("geometry_id"),"start_node":start,"departure_targets":entry_nodes,"departure_path":departure,
                             "arrival_targets":exit_nodes,"arrival_path":arrival,"status":status,
                             "message":"Arrival and departure routes exist." if status=="PASS" else f"{stand.get('geometry_id')}: source-backed arrival exit to stand or stand to runway entry route is missing."})
    stand_access = bool(stands) and all(attached(s) for s in stands) and bool(stand_routes) and all(r["status"]=="PASS" for r in stand_routes)
    gse_routes=[]
    stand_nodes=sorted({s.get("properties",{}).get("network_node_id") for s in stands})
    for depot in sorted(depots,key=lambda x:x.get("geometry_id","")):
        start=depot.get("properties",{}).get("network_node_id")
        route=next((_find_path(graph,start,target,"gse") for target in stand_nodes if start in nodes and _find_path(graph,start,target,"gse")),None)
        gse_routes.append({"feature_id":depot.get("geometry_id"),"start_node":start,"target_nodes":stand_nodes,"path":route,
                           "status":"PASS" if attached(depot) and route else "FAIL",
                           "message":"Source-backed GSE route exists." if attached(depot) and route else f"{depot.get('geometry_id')}: depot is disconnected from the service-road/apron network or stand access."})
    gse_access = bool(depots) and all(attached(s) for s in depots) and bool(gse_routes) and all(r["status"]=="PASS" for r in gse_routes)
    runway_connections=bool(exit_nodes) and bool(entry_nodes)
    turnaround_relations=[r for r in relations if r.get("relation_type")=="turnaround_access" and r.get("source_id") in source_ids]
    turnaround_results=[]
    for stand in sorted(stands,key=lambda x:x.get("geometry_id","")):
        node=stand.get("properties",{}).get("turnaround_node_id")
        relation=next((r for r in turnaround_relations if r.get("feature_id")==stand.get("geometry_id") and r.get("to_node")==node),None)
        route=_find_path(graph,stand.get("properties",{}).get("network_node_id"),node,"aircraft") if node in nodes else None
        turnaround_results.append({"feature_id":stand.get("geometry_id"),"node_id":node,"path":route,
                                   "status":"PASS" if relation and route else "FAIL",
                                   "message":"Source-backed turnaround/pushback access exists." if relation and route else f"{stand.get('geometry_id')}: turnaround or pushback connectivity is not source-backed."})
    turnaround_ok=bool(stands) and bool(turnaround_results) and all(r["status"]=="PASS" for r in turnaround_results)
    clearance_types={row.get("assessment_type") for row in dataset.get("clearance_assessments", [])}
    required_clearance_types={"aircraft_route","aircraft_vs_gse"}
    edges=graph.get("edges",[])
    known_widths=bool(edges) and all(_finite_number(e.get("width_m")) and float(e["width_m"])>0 for e in edges)
    clearance = "FAIL" if "FAIL" in clearance_statuses else "PASS" if clearance_rows and clearance_statuses=={"PASS"} and required_clearance_types.issubset(clearance_types) and known_widths else "UNKNOWN"
    valid = not valid_findings
    runway_features=by_type.get("runway",[])
    required_runway_ids=set(dataset.get("required_runway_ids",[])) or {f.get("properties",{}).get("runway_id") for f in runway_features}
    runway_ids={f.get("properties",{}).get("runway_id") for f in runway_features}
    thresholds={}
    for feature in by_type.get("runway_threshold",[]):
        thresholds.setdefault(feature.get("properties",{}).get("runway_id"),set()).add(feature.get("properties",{}).get("threshold_id"))
    runway_geometry_ok=bool(required_runway_ids) and required_runway_ids.issubset(runway_ids) and all(
        set(str(runway_id).split("/" )).issubset(thresholds.get(runway_id,set())) for runway_id in required_runway_ids)
    reqs = [
        _requirement("REFERENCE_GEOMETRY", "PASS" if by_type.get("airport_reference_point") else "BLOCKED", by_type.get("airport_reference_point", []), "Airport reference point is supplied." if by_type.get("airport_reference_point") else "No airport reference point is supplied."),
        _requirement("RUNWAY_GEOMETRY", "PASS" if runway_geometry_ok else "BLOCKED", by_type.get("runway", []) + by_type.get("runway_threshold", []), "Required runway axes and both threshold ends are present." if runway_geometry_ok else "A required runway centerline, identifier, or matching pair of threshold ends is missing."),
        _requirement("TAXIWAY_GEOMETRY", "PASS" if taxiways else "BLOCKED", taxiways, "Taxiway centerline geometry is present." if taxiways else "No taxiway centerlines are supplied."),
        _requirement("TAXIWAY_CONNECTIVITY", "PASS" if graph.get("edges") and not any(i["code"] == "DISCONNECTED_COMPONENTS" for i in graph_issues) else "BLOCKED", graph.get("edges", []), "Routing graph is connected." if graph.get("edges") and not any(i["code"] == "DISCONNECTED_COMPONENTS" for i in graph_issues) else "Routing graph is absent or disconnected.", details=graph_issues),
        _requirement("RUNWAY_CONNECTIONS", "PASS" if runway_connections else "BLOCKED", runway_relations, "Source-backed runway entry/exit relationships are present." if runway_connections else "No source-backed runway entry/exit relationship is supplied."),
        _requirement("STAND_ACCESS", "PASS" if stand_access else "BLOCKED", stands, "All stands have source-backed routes to runway entry/exit nodes." if stand_access else "Stand geometry or a source-backed route to a runway entry/exit is missing.", details=stand_routes),
        _requirement("GSE_ACCESS", "PASS" if gse_access else "BLOCKED", depots, "All GSE depots connect to the stand/service network." if gse_access else "GSE staging/depot access or a connected GSE route to a stand is missing.", details=gse_routes),
        _requirement("SERVICE_ROADS", "PASS" if roads else "BLOCKED", roads, "Service-road geometry is supplied." if roads else "Service-road geometry is missing."),
        _requirement("PUSHBACK_TURNAROUND", "PASS" if turnaround_ok else "BLOCKED", stands, "Every stand has a source-backed turnaround/pushback link." if turnaround_ok else "Stand pushback/turnaround connectivity is missing or unsupported.", details=turnaround_results),
        _requirement("RUNWAY_CROSSINGS", crossing_status, crossings, "All routed runway crossings have explicit permission evidence." if crossing_status=="PASS" else "No routed runway crossing requires a crossing permission." if crossing_status=="NOT_APPLICABLE" else "A runway intersection lacks explicit source-backed permission geometry." if crossing_intersections else "No routing graph or runway-crossing permission geometry is supplied.",details=crossing_intersections),
        _requirement("ROUTE_CLEARANCES", clearance, clearance_inputs, "All required clearance assessments pass and every route edge has source-backed width." if clearance == "PASS" else "One or more measured clearances fail." if clearance == "FAIL" else "Required width/boundary or clearance evidence is insufficient."),
        _requirement("SOURCE_AUTHORITY", "PASS" if authority_ok else "BLOCKED", features, f"Active feature authorities satisfy the {profile} policy." if authority_ok else f"One or more active features are not approved under the {profile} authority policy."),
        _requirement("PROVENANCE_COMPLETENESS", "PASS" if valid else "FAIL", features, "Feature provenance and schema are complete." if valid else "Canonical schema/provenance validation found errors.", details=valid_findings),
        _requirement("GRAPH_VALIDITY", "PASS" if graph.get("edges") and not blocking_graph_issues else "BLOCKED", graph.get("edges", []), "Graph has source lineage and no blocking topology diagnostics." if graph.get("edges") and not blocking_graph_issues else "Graph is missing or has blocking topology diagnostics.", details=graph_issues),
    ]
    allowed_statuses = {"PASS", "NOT_APPLICABLE"}
    ready = valid and all(r["status"] in allowed_statuses for r in reqs)
    if ready and profile == "production": decision = "SIMULATOR_READY"
    elif ready: decision = "SIMULATOR_READY"
    else: decision = "DATA_BLOCKED"
    return {"schema_version": "1.0", "airport_id": dataset.get("airport", {}).get("airport_id"),
            "profile": profile, "execution_scope": "PRODUCTION" if profile == "production" else "TEST_FIXTURE_ONLY",
            "decision": decision, "production_simulator_allowed": decision == "SIMULATOR_READY" and profile == "production",
            "requirements": reqs, "graph_diagnostics": graph_issues, "validation_findings": valid_findings,
            "topology_routes": {"stand_to_runway":stand_routes,"gse_to_stand":gse_routes,"turnaround":turnaround_results},
            "clearance_assessments": clearance_rows,
            "feature_counts": {kind: len(by_type.get(kind, [])) for kind in sorted(FEATURE_TYPES)},
            "quarantined_feature_count": len(dataset.get("quarantined_features", []))}


def _requirement(name: str, status: str, features: list[Any], reason: str, *, details: Any = None) -> dict[str, Any]:
    return {"requirement": name, "status": status, "supporting_feature_ids": sorted(
        str(x.get("geometry_id", x.get("edge_id", x.get("relation_id", x.get("assessment_id", ""))))) for x in features if isinstance(x, dict)),
        "reason": reason, "missing_information": [] if status in {"PASS","NOT_APPLICABLE"} else [reason],
        "source_ids": sorted({x.get("provenance", {}).get("source_id") or x.get("source_id") for x in features if isinstance(x, dict) and (x.get("provenance", {}).get("source_id") or x.get("source_id"))}),
        "details": details or []}


def compare_datasets(before: dict[str, Any], after: dict[str, Any], move_tolerance_m: float = 0.01, *, profile: str = "production") -> dict[str, Any]:
    def index(ds): return {f["geometry_id"]: f for f in ds.get("features", [])}
    old, new = index(before), index(after)
    added = sorted(set(new)-set(old)); removed = sorted(set(old)-set(new)); changes = []
    for identity in sorted(set(old) & set(new)):
        a, b = old[identity], new[identity]
        fields = []
        if a.get("feature_type") != b.get("feature_type"): fields.append({"change": "identifier_or_type", "before": a.get("feature_type"), "after": b.get("feature_type")})
        pa, pb = a.get("provenance", {}), b.get("provenance", {})
        if pa.get("authority") != pb.get("authority") or pa.get("source_id") != pb.get("source_id"): fields.append({"change": "authority_provenance", "before": [pa.get("authority"), pa.get("source_id")], "after": [pb.get("authority"), pb.get("source_id")]})
        if a.get("properties") != b.get("properties"): fields.append({"change": "properties_permissions_clearances", "before": a.get("properties"), "after": b.get("properties")})
        if a.get("coordinates") != b.get("coordinates"):
            d = _geometry_displacement(a, b)
            if d is None or d > move_tolerance_m: fields.append({"change": "moved", "maximum_vertex_displacement_m": d})
        if fields: changes.append({"geometry_id": identity, "changes": fields})
    old_rel = {json.dumps(r, sort_keys=True) for r in before.get("operational_relationships", [])}
    new_rel = {json.dumps(r, sort_keys=True) for r in after.get("operational_relationships", [])}
    old_edge = {e.get("edge_id"): e for e in before.get("operational_graph", {}).get("edges", [])}
    new_edge = {e.get("edge_id"): e for e in after.get("operational_graph", {}).get("edges", [])}
    old_readiness, new_readiness = readiness(before,profile=profile), readiness(after,profile=profile)
    return {"schema_version": "1.0", "before_dataset_id": before.get("dataset_id"), "after_dataset_id": after.get("dataset_id"),
            "added_features": added, "removed_features": removed, "changed_features": changes,
            "changed_connectivity": {"added_relationships": sorted(new_rel-old_rel), "removed_relationships": sorted(old_rel-new_rel),
                                     "changed_edges": sorted(e for e in set(old_edge)&set(new_edge) if old_edge[e] != new_edge[e])},
            "readiness_profile":profile,"readiness_change": {"before": old_readiness["decision"], "after": new_readiness["decision"]}}


def _geometry_displacement(a: dict[str, Any], b: dict[str, Any]) -> float | None:
    try:
        pa = list(_positions(a["coordinates"], a["geometry_type"])); pb = list(_positions(b["coordinates"], b["geometry_type"]))
        if len(pa) != len(pb): return None
        return round(max((_distance(x, y) for x, y in zip(pa, pb)), default=0.0), 6)
    except (KeyError, TypeError, ValueError): return None


def render_svg(dataset: dict[str, Any], report: dict[str, Any] | None = None) -> str:
    features = dataset.get("features", []) + dataset.get("quarantined_features", [])
    all_points = []
    for f in features:
        if f.get("geometry_type") in GEOMETRIES:
            all_points.extend(_positions(f.get("coordinates", []), f.get("geometry_type")))
    all_points.extend(n.get("coordinates") for n in dataset.get("operational_graph",{}).get("nodes",[]) if isinstance(n.get("coordinates"),list))
    if not all_points: all_points = [[0, 0]]
    xs, ys = [p[0] for p in all_points], [p[1] for p in all_points]
    xmin, xmax, ymin, ymax = min(xs), max(xs), min(ys), max(ys)
    scale = min(900/max(xmax-xmin, 1), 650/max(ymax-ymin, 1))
    def xy(p): return 40+(p[0]-xmin)*scale, 690-(p[1]-ymin)*scale
    colors = {"AUTHORITATIVE":"#157a52", "SURVEYED":"#157a52", "OPERATOR_PROVIDED":"#2379b8", "SOURCE_DERIVED":"#3583bd", "APPROXIMATE":"#d97904", "SYNTHETIC":"#8b5cf6", "UNKNOWN":"#7c8795"}
    elements = [f'<svg xmlns="http://www.w3.org/2000/svg" width="1000" height="740" viewBox="0 0 1000 740">',
                '<rect width="100%" height="100%" fill="#fbfcfe"/>',
                f'<text x="40" y="28" font-family="Arial" font-size="18">{_escape(str(dataset.get("airport",{}).get("airport_id","Airport")))} geometry inspection</text>']
    for f in sorted(features, key=lambda x: (x in dataset.get("quarantined_features", []), x.get("geometry_id", ""))):
        pts = list(_positions(f.get("coordinates", []), f.get("geometry_type")))
        if not pts: continue
        authority = f.get("provenance", {}).get("authority", "UNKNOWN")
        color = colors.get(authority, "#7c8795"); dashed = " stroke-dasharray='5 4'" if authority in {"APPROXIMATE", "SYNTHETIC"} or f in dataset.get("quarantined_features", []) else ""
        if f.get("geometry_type") in {"LineString", "MultiLineString", "Polygon", "MultiPolygon"}:
            if f["geometry_type"]=="LineString": lines=[pts]
            elif f["geometry_type"]=="MultiLineString": lines=f["coordinates"]
            elif f["geometry_type"]=="Polygon": lines=f["coordinates"]
            else: lines=[ring for poly in f["coordinates"] for ring in poly]
            for line in lines:
                xylist = [xy(p) for p in line]
                points_attr=" ".join(f"{x:.2f},{y:.2f}" for x,y in xylist)
                if f.get("geometry_type") in {"Polygon","MultiPolygon"}:
                    elements.append(f'<polygon points="{points_attr}" fill="{color}" fill-opacity="0.12" stroke="{color}" stroke-width="2"{dashed}/>')
                else:
                    elements.append(f'<polyline points="{points_attr}" fill="none" stroke="{color}" stroke-width="3"{dashed}/>')
        elif f.get("geometry_type") == "Point":
            x,y=xy(pts[0]); elements.append(f'<circle cx="{x:.2f}" cy="{y:.2f}" r="4" fill="{color}"/>')
            elements.append(f'<title>{_escape(f.get("geometry_id", ""))} · {_escape(authority)}</title>')
    graph=dataset.get("operational_graph",{}); node_map={n.get("node_id"):n for n in graph.get("nodes",[])}
    for edge in graph.get("edges",[]):
        a=node_map.get(edge.get("from_node"),{}).get("coordinates"); b=node_map.get(edge.get("to_node"),{}).get("coordinates")
        if a and b:
            x1,y1=xy(a); x2,y2=xy(b)
            elements.append(f'<line x1="{x1:.2f}" y1="{y1:.2f}" x2="{x2:.2f}" y2="{y2:.2f}" stroke="#202833" stroke-width="1.5" stroke-dasharray="2 3"/>')
    for node in graph.get("nodes",[]):
        if node.get("coordinates"):
            x,y=xy(node["coordinates"]); elements.append(f'<circle cx="{x:.2f}" cy="{y:.2f}" r="2.5" fill="#202833"/>')
            elements.append(f'<text x="{x+4:.2f}" y="{y-4:.2f}" font-family="Arial" font-size="8">{_escape(node.get("node_id", ""))}</text>')
    if report:
        failed_ids={fid for req in report.get("requirements",[]) if req.get("status") not in {"PASS","NOT_APPLICABLE"} for fid in req.get("supporting_feature_ids",[])}
        for f in features:
            if f.get("geometry_id") in failed_ids and f.get("geometry_type")=="Point":
                x,y=xy(f["coordinates"]); elements.append(f'<circle cx="{x:.2f}" cy="{y:.2f}" r="9" fill="none" stroke="#c62828" stroke-width="2"/>')
    elements.extend(['<text x="40" y="718" font-family="Arial" font-size="11">Green: authoritative/surveyed; blue: operator/source-derived; orange dashed: approximate/quarantined; purple: synthetic fixture.</text>', '</svg>'])
    return "\n".join(elements)+"\n"


def _escape(value: str) -> str:
    return value.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace('"', "&quot;")


def markdown_readiness(report: dict[str, Any]) -> str:
    lines = [f"# {report.get('airport_id')} geometry readiness", "", f"**Decision: {report['decision']}**", "",
             f"Profile: `{report['profile']}`; execution scope: `{report['execution_scope']}`.", "",
             "| Requirement | Status | Reason |", "|---|---|---|"]
    for row in report["requirements"]:
        reason = row["reason"].replace("|", "\\|")
        lines.append(f"| {row['requirement']} | {row['status']} | {reason} |")
    route_rows=report.get("topology_routes",{})
    for title,key in (("Stand to runway routes","stand_to_runway"),("GSE to stand routes","gse_to_stand"),("Turnaround routes","turnaround")):
        if route_rows.get(key):
            lines.extend(["",f"## {title}",""])
            for route in route_rows[key]:
                parts=[]
                if route.get("arrival_path") is not None: parts.append("arrival "+" → ".join(route["arrival_path"]))
                if route.get("departure_path") is not None: parts.append("departure "+" → ".join(route["departure_path"]))
                if route.get("path") is not None: parts.append("path "+" → ".join(route["path"]))
                path="; ".join(parts) or "no path"
                lines.append(f"- **{route['feature_id']} — {route['status']}**: {route['message']} Path: `{path}`.")
    return "\n".join(lines)+"\n"
