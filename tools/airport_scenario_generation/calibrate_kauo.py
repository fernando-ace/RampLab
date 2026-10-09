"""Rebuild KAUO geometry from FAA runway-end coordinates and report errors."""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
from typing import Any

from .geospatial import (
    GeographicCoordinate,
    LocalEnu,
    enu_to_geographic,
    geographic_to_enu,
    rotate_local_grid_to_enu,
    true_bearing_unit_vector,
)

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_DIRECTORY = Path(__file__).resolve().parent


def _geo(latitude: float, longitude: float, height: float) -> GeographicCoordinate:
    return GeographicCoordinate(latitude, longitude, height)


def _enu_dict(point: LocalEnu) -> dict[str, float]:
    return {"x": round(point.east_m, 6), "y": round(point.north_m, 6)}


def _bearing(start: LocalEnu, end: LocalEnu) -> float:
    return math.degrees(math.atan2(end.east_m - start.east_m, end.north_m - start.north_m)) % 360.0


def _line_intersection(a: LocalEnu, b: LocalEnu, c: LocalEnu, d: LocalEnu) -> LocalEnu:
    ab_x, ab_y = b.east_m - a.east_m, b.north_m - a.north_m
    cd_x, cd_y = d.east_m - c.east_m, d.north_m - c.north_m
    denominator = ab_x * cd_y - ab_y * cd_x
    if abs(denominator) < 1e-12:
        raise ValueError("runway centerlines are parallel; no unique intersection")
    t = ((c.east_m - a.east_m) * cd_y - (c.north_m - a.north_m) * cd_x) / denominator
    return LocalEnu(a.east_m + t * ab_x, a.north_m + t * ab_y)


def _transform_legacy_node(node: dict[str, Any], legacy: dict[str, Any], origin: GeographicCoordinate) -> LocalEnu:
    old_origin = _geo(
        legacy["origin_latitude_degrees"], legacy["origin_longitude_degrees"],
        legacy["origin_ellipsoid_height_m"],
    )
    old_enu = rotate_local_grid_to_enu(
        float(node["x_m"]), float(node["y_m"]),
        positive_x_true_bearing_degrees=float(legacy["positive_x_true_bearing_degrees"]),
        origin_offset_east_m=float(legacy["offset_east_m"]),
        origin_offset_north_m=float(legacy["offset_north_m"]),
    )
    old_geographic = enu_to_geographic(old_enu, old_origin)
    return geographic_to_enu(old_geographic, origin)


def _baseline_errors(anchors: dict[str, Any], origin: GeographicCoordinate) -> list[dict[str, Any]]:
    legacy = anchors["approximation_policy"]["legacy_frame"]
    expected_by_id = {
        threshold_id: geographic_to_enu(_geo(
            float(value["latitude_degrees"]), float(value["longitude_degrees"]), origin.ellipsoid_height_m
        ), origin)
        for runway in anchors["runways"] for threshold_id, value in runway["thresholds"].items()
    }
    errors = []
    for runway in anchors["runways"]:
        feature = next(row for row in anchors["goal25_baseline_runways"] if row["identifier"] == runway["identifier"])
        center = _transform_legacy_node({
            "x_m": feature["center_local_m"]["x"], "y_m": feature["center_local_m"]["y"]
        }, legacy, origin)
        east, north = true_bearing_unit_vector(float(feature["true_heading_degrees_from_runway18"]))
        half_length = float(feature["length_m"]) * 0.5
        threshold_18 = LocalEnu(center.east_m - east * half_length, center.north_m - north * half_length)
        threshold_36 = LocalEnu(center.east_m + east * half_length, center.north_m + north * half_length)
        endpoint_names = list(runway["thresholds"])
        for label, generated in zip(endpoint_names, (threshold_18, threshold_36)):
            expected = expected_by_id[label]
            errors.append({
                "runway": runway["identifier"], "threshold": label,
                "error_m": round(math.hypot(generated.east_m - expected.east_m, generated.north_m - expected.north_m), 3),
            })
    return errors


def calibrate(anchor_path: Path, geometry_path: Path, report_path: Path) -> dict[str, Any]:
    anchors = json.loads(anchor_path.read_text(encoding="utf-8"))
    old_geometry = json.loads(geometry_path.read_text(encoding="utf-8"))
    origin_data = anchors["origin"]
    origin = _geo(origin_data["latitude_degrees"], origin_data["longitude_degrees"], origin_data["ellipsoid_height_m"])
    airport = old_geometry["airport"]
    feature_data = airport["features"]
    legacy = anchors["approximation_policy"]["legacy_frame"]
    already_enu = airport.get("calibration", {}).get("coordinate_reference_system") == anchors["coordinate_reference_system"]
    generated_node_ids = {"rwy18_threshold", "rwy11_threshold", "rwy29_threshold", "runway_intersection"}
    airport["nodes"] = [node for node in airport["nodes"] if node["id"] not in generated_node_ids]

    convert_approximate = lambda point: (
        LocalEnu(float(point["x_m"]), float(point["y_m"])) if already_enu
        else _transform_legacy_node(point, legacy, origin)
    )
    positions = {node["id"]: convert_approximate(node) for node in airport["nodes"]}
    thresholds: dict[str, LocalEnu] = {}
    runway_rows: list[dict[str, Any]] = []
    tolerance = float(anchors["position_tolerance_m"])
    anchor_rows = []
    for runway in anchors["runways"]:
        runway_ids = list(runway["thresholds"])
        points = {}
        for runway_end_id, source_point in runway["thresholds"].items():
            coordinate = _geo(
                float(source_point["latitude_degrees"]),
                float(source_point["longitude_degrees"]), origin.ellipsoid_height_m,
            )
            local = geographic_to_enu(coordinate, origin)
            local = LocalEnu(round(local.east_m, 6), round(local.north_m, 6))
            points[runway_end_id] = local
            thresholds[runway_end_id] = local
            positions[f"rwy{runway_end_id}_threshold"] = local
            reconstructed = enu_to_geographic(local, origin)
            error = geographic_to_enu(reconstructed, coordinate)
            error_m = math.hypot(error.east_m, error.north_m)
            anchor_rows.append({
                "id": f"rwy{runway_end_id}_threshold", "runway": runway["identifier"],
                "classification": "AUTHORITATIVE", "source_latitude_degrees": coordinate.latitude_degrees,
                "source_longitude_degrees": coordinate.longitude_degrees,
                "source_record": source_point["source_record"],
                "generated_enu_m": _enu_dict(local),
                "generated_latitude_degrees": round(reconstructed.latitude_degrees, 10),
                "generated_longitude_degrees": round(reconstructed.longitude_degrees, 10),
                "position_error_m": round(error_m, 6), "tolerance_m": tolerance,
                "status": "PASS" if error_m <= tolerance else "FAIL",
            })
        start, end = points[runway_ids[0]], points[runway_ids[1]]
        center = LocalEnu((start.east_m + end.east_m) / 2.0, (start.north_m + end.north_m) / 2.0)
        length = math.hypot(end.east_m - start.east_m, end.north_m - start.north_m)
        runway_rows.append({
            "identifier": runway["identifier"], "length_m": round(length, 6),
            "width_m": runway["published_width_m"],
            "true_heading_degrees_from_first_threshold": round(_bearing(start, end), 6),
            "center_local_m": _enu_dict(center), "published_length_m": runway["published_length_m"],
            "source_classification": "AUTHORITATIVE endpoints / DERIVED centerline and heading",
        })
        positions[f"runway_{runway['identifier'].replace('/', '_')}_mid"] = center
        if runway["identifier"] == "18/36":
            positions["rwy18_36_mid"] = center
            positions["rwy36_hold"] = thresholds["36"]

    intersection = _line_intersection(thresholds["18"], thresholds["36"], thresholds["11"], thresholds["29"])
    positions["runway_intersection"] = intersection
    positions_by_id = {node["id"]: positions[node["id"]] for node in airport["nodes"]}
    for node in airport["nodes"]:
        point = positions_by_id[node["id"]]
        node["x_m"], node["y_m"] = round(point.east_m, 6), round(point.north_m, 6)
    for runway_end_id in ("18", "11", "29"):
        point = thresholds[runway_end_id]
        airport["nodes"].append({
            "id": f"rwy{runway_end_id}_threshold",
            "name": f"Runway {runway_end_id} FAA NASR surveyed runway end",
            "x_m": round(point.east_m, 6), "y_m": round(point.north_m, 6),
        })
    airport["nodes"].append({
        "id": "runway_intersection", "name": "Derived runway centerline intersection",
        "x_m": round(intersection.east_m, 6), "y_m": round(intersection.north_m, 6),
    })

    feature_data["runways"] = runway_rows
    feature_data["runway_intersection_local_m"] = _enu_dict(intersection)
    feature_data["runway_intersection_classification"] = "DERIVED from authoritative FAA NASR runway-end coordinates"
    apron = feature_data["apron_center_local_m"]
    apron_point = (
        LocalEnu(float(apron["x"]), float(apron["y"])) if already_enu
        else _transform_legacy_node({"x_m": apron["x"], "y_m": apron["y"]}, legacy, origin)
    )
    feature_data["apron_center_local_m"] = _enu_dict(apron_point)
    feature_data["apron_classification"] = "APPROXIMATE chart-graticule reference; not surveyed"
    for building in feature_data.get("buildings", []):
        center = building["center_local_m"]
        building["center_local_m"] = _enu_dict(
            LocalEnu(float(center["x"]), float(center["y"])) if already_enu
            else _transform_legacy_node({"x_m": center["x"], "y_m": center["y"]}, legacy, origin)
        )
    airport["calibration"] = {
        "coordinate_reference_system": anchors["coordinate_reference_system"],
        "origin": origin_data,
        "scenario_axis_convention": "+X east, +Y north, meters; these are airport-local ENU coordinates.",
        "runway_thresholds": {key: _enu_dict(value) for key, value in thresholds.items()},
        "source_classification": "Runway-end coordinates AUTHORITATIVE; runway centers/headings and intersection DERIVED; taxiway/apron/parking features APPROXIMATE.",
    }
    # The taxi graph is drawn as straight segments, so lengths and traversal
    # times are kept consistent with the regenerated ENU node positions.
    node_positions = {node["id"]: (node["x_m"], node["y_m"]) for node in airport["nodes"]}
    for edge in airport["edges"]:
        a, b = node_positions[edge["from"]], node_positions[edge["to"]]
        distance = math.hypot(b[0] - a[0], b[1] - a[1])
        edge["distance_m"] = round(distance, 3)
    # Preserve the Goal 25 path-speed assumption as the source of graph times.
    speed = float(old_geometry["surface_operations"]["aircraft_speed_mps"])
    for edge in airport["edges"]:
        edge["traversal_time_seconds"] = math.ceil(float(edge["distance_m"]) / speed)
    feature_data["kauo_calibrated"] = True
    feature_data["geometry_status"] = "FAA surveyed runway ends; centerlines and intersection derived from those ends; taxiway/apron/parking nodes remain approximate chart-digitized ENU references."

    old_geometry["calibration"] = {
        "coordinate_reference_system": anchors["coordinate_reference_system"],
        "airport_reference_latitude": origin.latitude_degrees,
        "airport_reference_longitude": origin.longitude_degrees,
        "airport_reference_ellipsoid_height_m_approx": origin.ellipsoid_height_m,
        "runway_18_36_length_m": anchors["runways"][0]["published_length_m"],
        "runway_18_36_width_m": anchors["runways"][0]["published_width_m"],
        "runway_11_29_length_m": anchors["runways"][1]["published_length_m"],
        "runway_11_29_width_m": anchors["runways"][1]["published_width_m"],
        "runway_features": [
            {
                "id": "runway_" + row["identifier"].replace("/", "_"),
                "identifier": row["identifier"],
                "length_m": row["published_length_m"],
                "endpoint_distance_m": row["length_m"],
                "width_m": row["width_m"],
                "surface": source["surface"],
                "true_heading_degrees_from_first_threshold": row["true_heading_degrees_from_first_threshold"],
                "center_local_m": row["center_local_m"],
                "source_classification": row["source_classification"],
            }
            for row, source in zip(runway_rows, anchors["runways"])
        ],
        "simulator_local_axis_note": "RampLab scenario XY is airport-local ENU in meters (+X east, +Y north). The Cesium origin is the FAA NASR airport reference point; Unreal maps east to +X centimeters and north to -Y centimeters. No KAUO actor offset or rotation correction is applied.",
        "geometry_status": "FAA NASR runway ends are authoritative; runway centers, headings, and intersection are derived; taxiway/apron/parking nodes remain approximate chart-digitized references.",
    }

    baseline_errors = _baseline_errors(anchors, origin)
    geometry_endpoint_errors = []
    for runway, feature in zip(anchors["runways"], runway_rows):
        labels = list(runway["thresholds"])
        center = LocalEnu(feature["center_local_m"]["x"], feature["center_local_m"]["y"])
        east, north = true_bearing_unit_vector(feature["true_heading_degrees_from_first_threshold"])
        half_length = feature["length_m"] * 0.5
        endpoints = (
            LocalEnu(center.east_m - east * half_length, center.north_m - north * half_length),
            LocalEnu(center.east_m + east * half_length, center.north_m + north * half_length),
        )
        for label, generated in zip(labels, endpoints):
            expected = thresholds[label]
            error_m = math.hypot(generated.east_m - expected.east_m, generated.north_m - expected.north_m)
            geometry_endpoint_errors.append({
                "runway": runway["identifier"], "threshold": label,
                "generated_geometry_enu_m": _enu_dict(generated),
                "geometry_endpoint_error_m": round(error_m, 6), "tolerance_m": tolerance,
                "status": "PASS" if error_m <= tolerance else "FAIL",
            })
    report = {
        "schema_version": "1.0", "airport_id": "KAUO", "coordinate_reference_system": anchors["coordinate_reference_system"],
        "origin": origin_data, "tolerance_m": tolerance, "tolerance_note": anchors["tolerance_note"],
        "scenario_axis_convention": "+X east, +Y north, meters; Unreal uses +X east and -Y north in centimeters.",
        "authoritative_runway_thresholds": anchor_rows,
        "generated_runway_geometry_endpoint_errors": geometry_endpoint_errors,
        "derived_runway_intersection": {
            "classification": "DERIVED", "local_enu_m": _enu_dict(intersection),
            "geographic": enu_to_geographic(intersection, origin).__dict__,
            "source": anchors["derived_anchors"]["source"],
        },
        "baseline_goal25_threshold_errors": baseline_errors,
        "baseline_maximum_threshold_error_m": max(row["error_m"] for row in baseline_errors),
        "registration_maximum_threshold_error_m": max(row["geometry_endpoint_error_m"] for row in geometry_endpoint_errors),
        "classification_summary": {
            "AUTHORITATIVE": "FAA NASR runway-end coordinates and runway dimensions.",
            "DERIVED": "Runway midpoint, heading, and centerline intersection computed from FAA runway-end coordinates.",
            "APPROXIMATE": "Taxiway graph, apron, terminal/hangar, and parking references migrated from Goal 25 FAA chart-graticule digitization.",
            "ASSUMED": "Operational scenario timing, aircraft archetype, task duration, taxi/rollout/occupancy speeds and times remain the Goal 25 assumptions.",
        },
    }
    geometry_path.write_text(json.dumps(old_geometry, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    report_path.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    return report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--anchors", type=Path, default=DEFAULT_DIRECTORY / "kauo.anchors.json")
    parser.add_argument("--geometry", type=Path, default=DEFAULT_DIRECTORY / "kauo.geometry.json")
    parser.add_argument("--report", type=Path, default=DEFAULT_DIRECTORY / "kauo_alignment_validation.json")
    args = parser.parse_args(argv)
    report = calibrate(args.anchors, args.geometry, args.report)
    print(json.dumps({
        "geometry": str(args.geometry), "report": str(args.report),
        "baseline_maximum_threshold_error_m": report["baseline_maximum_threshold_error_m"],
        "registration_maximum_threshold_error_m": report["registration_maximum_threshold_error_m"],
    }, indent=2))
    return 0 if all(row["status"] == "PASS" for row in report["authoritative_runway_thresholds"]) else 1


if __name__ == "__main__":
    raise SystemExit(main())
