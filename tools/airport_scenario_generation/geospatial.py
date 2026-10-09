"""Deterministic WGS-84 and airport-local coordinate conversion helpers.

Airport scenario XY uses local ENU meters: +X east, +Y north. Heights in
``GeographicCoordinate`` are WGS-84 ellipsoid heights in meters.
"""
from __future__ import annotations

from dataclasses import dataclass
import math

WGS84_SEMI_MAJOR_M = 6_378_137.0
WGS84_INVERSE_FLATTENING = 298.257223563
WGS84_FLATTENING = 1.0 / WGS84_INVERSE_FLATTENING
WGS84_ECCENTRICITY_SQUARED = WGS84_FLATTENING * (2.0 - WGS84_FLATTENING)


@dataclass(frozen=True)
class GeographicCoordinate:
    latitude_degrees: float
    longitude_degrees: float
    ellipsoid_height_m: float = 0.0


@dataclass(frozen=True)
class LocalEnu:
    east_m: float
    north_m: float
    up_m: float = 0.0


def _validate_coordinate(coordinate: GeographicCoordinate) -> None:
    values = (coordinate.latitude_degrees, coordinate.longitude_degrees, coordinate.ellipsoid_height_m)
    if not all(math.isfinite(value) for value in values):
        raise ValueError("geographic coordinate values must be finite")
    if not -90.0 <= coordinate.latitude_degrees <= 90.0:
        raise ValueError("latitude must be between -90 and 90 degrees")
    if not -180.0 <= coordinate.longitude_degrees <= 180.0:
        raise ValueError("longitude must be between -180 and 180 degrees")


def _geodetic_to_ecef(coordinate: GeographicCoordinate) -> tuple[float, float, float]:
    _validate_coordinate(coordinate)
    latitude = math.radians(coordinate.latitude_degrees)
    longitude = math.radians(coordinate.longitude_degrees)
    sin_latitude, cos_latitude = math.sin(latitude), math.cos(latitude)
    prime_vertical_radius = WGS84_SEMI_MAJOR_M / math.sqrt(
        1.0 - WGS84_ECCENTRICITY_SQUARED * sin_latitude * sin_latitude
    )
    radial = prime_vertical_radius + coordinate.ellipsoid_height_m
    return (
        radial * cos_latitude * math.cos(longitude),
        radial * cos_latitude * math.sin(longitude),
        (prime_vertical_radius * (1.0 - WGS84_ECCENTRICITY_SQUARED) + coordinate.ellipsoid_height_m)
        * sin_latitude,
    )


def geographic_to_enu(coordinate: GeographicCoordinate, origin: GeographicCoordinate) -> LocalEnu:
    """Convert WGS-84 latitude/longitude/ellipsoid height to local ENU meters."""
    x, y, z = _geodetic_to_ecef(coordinate)
    ox, oy, oz = _geodetic_to_ecef(origin)
    dx, dy, dz = x - ox, y - oy, z - oz
    latitude, longitude = math.radians(origin.latitude_degrees), math.radians(origin.longitude_degrees)
    sin_latitude, cos_latitude = math.sin(latitude), math.cos(latitude)
    sin_longitude, cos_longitude = math.sin(longitude), math.cos(longitude)
    return LocalEnu(
        -sin_longitude * dx + cos_longitude * dy,
        -sin_latitude * cos_longitude * dx - sin_latitude * sin_longitude * dy + cos_latitude * dz,
        cos_latitude * cos_longitude * dx + cos_latitude * sin_longitude * dy + sin_latitude * dz,
    )


def enu_to_geographic(point: LocalEnu, origin: GeographicCoordinate) -> GeographicCoordinate:
    """Convert local ENU meters to WGS-84 latitude/longitude/ellipsoid height."""
    if not all(math.isfinite(value) for value in (point.east_m, point.north_m, point.up_m)):
        raise ValueError("ENU coordinate values must be finite")
    ox, oy, oz = _geodetic_to_ecef(origin)
    latitude, longitude = math.radians(origin.latitude_degrees), math.radians(origin.longitude_degrees)
    sin_latitude, cos_latitude = math.sin(latitude), math.cos(latitude)
    sin_longitude, cos_longitude = math.sin(longitude), math.cos(longitude)
    x = ox - sin_longitude * point.east_m - sin_latitude * cos_longitude * point.north_m + cos_latitude * cos_longitude * point.up_m
    y = oy + cos_longitude * point.east_m - sin_latitude * sin_longitude * point.north_m + cos_latitude * sin_longitude * point.up_m
    z = oz + cos_latitude * point.north_m + sin_latitude * point.up_m

    longitude_result = math.atan2(y, x)
    horizontal_radius = math.hypot(x, y)
    latitude_result = math.atan2(z, horizontal_radius * (1.0 - WGS84_ECCENTRICITY_SQUARED))
    height = 0.0
    for _ in range(12):
        sin_latitude_result = math.sin(latitude_result)
        prime_vertical_radius = WGS84_SEMI_MAJOR_M / math.sqrt(
            1.0 - WGS84_ECCENTRICITY_SQUARED * sin_latitude_result * sin_latitude_result
        )
        height = horizontal_radius / math.cos(latitude_result) - prime_vertical_radius
        updated = math.atan2(
            z,
            horizontal_radius
            * (1.0 - WGS84_ECCENTRICITY_SQUARED * prime_vertical_radius / (prime_vertical_radius + height)),
        )
        if abs(updated - latitude_result) < 1e-14:
            latitude_result = updated
            break
        latitude_result = updated
    return GeographicCoordinate(math.degrees(latitude_result), math.degrees(longitude_result), height)


def true_bearing_unit_vector(bearing_degrees: float) -> tuple[float, float]:
    """Return east/north components for a true bearing measured clockwise from north."""
    if not math.isfinite(bearing_degrees):
        raise ValueError("bearing must be finite")
    bearing = math.radians(bearing_degrees)
    return math.sin(bearing), math.cos(bearing)


def rotate_local_grid_to_enu(
    east_like_x_m: float,
    north_like_y_m: float,
    *,
    positive_x_true_bearing_degrees: float,
    origin_offset_east_m: float = 0.0,
    origin_offset_north_m: float = 0.0,
) -> LocalEnu:
    """Map a right-handed local grid whose +X has a specified true bearing to ENU.

    This supports explicitly declared legacy or airport-specific grids. New
    scenario coordinates should already be ENU and need no rotation/offset.
    """
    if not all(math.isfinite(value) for value in (
        east_like_x_m, north_like_y_m, positive_x_true_bearing_degrees,
        origin_offset_east_m, origin_offset_north_m,
    )):
        raise ValueError("local frame values must be finite")
    radians = math.radians(positive_x_true_bearing_degrees)
    east = east_like_x_m * math.sin(radians) - north_like_y_m * math.cos(radians)
    north = east_like_x_m * math.cos(radians) + north_like_y_m * math.sin(radians)
    return LocalEnu(east + origin_offset_east_m, north + origin_offset_north_m)


def enu_to_unreal_centimeters(point: LocalEnu) -> tuple[float, float, float]:
    """Map ENU meters into the viewer's Unreal axes (+X east, -Y north)."""
    return point.east_m * 100.0, -point.north_m * 100.0, point.up_m * 100.0
