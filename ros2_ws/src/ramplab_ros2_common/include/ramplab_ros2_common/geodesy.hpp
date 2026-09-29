#pragma once

#include <cmath>

namespace ramplab_ros2_common {

// RampLab's +x east / +y north airport-local plane is mapped to a local
// WGS84 tangent plane at the KAUO reference point. The airport map is only a
// few hundred metres across, so the ellipsoidal local tangent approximation
// keeps curvature error negligible for this simulated apron.
struct GeodeticPoint {
  double latitude_deg{};
  double longitude_deg{};
  double altitude_m{};
};

struct LocalPoint {
  double east_m{};
  double north_m{};
  double up_m{};
};

inline constexpr double kAirportLatitudeDeg = 32.6151667;
inline constexpr double kAirportLongitudeDeg = -85.4340000;
inline constexpr double kAirportEllipsoidHeightM = 208.27;

inline GeodeticPoint local_to_geodetic(LocalPoint local) {
  constexpr double a = 6378137.0;
  constexpr double inverse_flattening = 298.257223563;
  constexpr double pi = 3.14159265358979323846;
  const double flattening = 1.0 / inverse_flattening;
  const double eccentricity2 = flattening * (2.0 - flattening);
  const double lat0 = kAirportLatitudeDeg * pi / 180.0;
  const double sin_lat = std::sin(lat0);
  const double denominator = std::sqrt(1.0 - eccentricity2 * sin_lat * sin_lat);
  const double prime_vertical = a / denominator;
  const double meridional = a * (1.0 - eccentricity2) /
      (denominator * denominator * denominator);
  const double height = kAirportEllipsoidHeightM + local.up_m;
  return {
    kAirportLatitudeDeg + local.north_m / (meridional + height) * 180.0 / pi,
    kAirportLongitudeDeg + local.east_m /
        ((prime_vertical + height) * std::cos(lat0)) * 180.0 / pi,
    height
  };
}

inline LocalPoint geodetic_to_local(GeodeticPoint point) {
  constexpr double a = 6378137.0;
  constexpr double inverse_flattening = 298.257223563;
  constexpr double pi = 3.14159265358979323846;
  const double flattening = 1.0 / inverse_flattening;
  const double eccentricity2 = flattening * (2.0 - flattening);
  const double lat0 = kAirportLatitudeDeg * pi / 180.0;
  const double sin_lat = std::sin(lat0);
  const double denominator = std::sqrt(1.0 - eccentricity2 * sin_lat * sin_lat);
  const double prime_vertical = a / denominator;
  const double meridional = a * (1.0 - eccentricity2) /
      (denominator * denominator * denominator);
  const double height = kAirportEllipsoidHeightM;
  return {
    (point.longitude_deg - kAirportLongitudeDeg) * pi / 180.0 *
        (prime_vertical + height) * std::cos(lat0),
    (point.latitude_deg - kAirportLatitudeDeg) * pi / 180.0 *
        (meridional + height),
    point.altitude_m - height
  };
}

}  // namespace ramplab_ros2_common
