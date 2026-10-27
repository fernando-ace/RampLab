#pragma once

#include "airside/world/airport_graph.hpp"
#include "airside/world/gate.hpp"

#include <string>
#include <vector>

namespace airside {

struct AirportSurfaceRegion {
    std::string id;
    std::string kind;
    std::string surface_type;
    double elevation_m{0.0};
    std::vector<Point2d> footprint;
    auto operator<=>(const AirportSurfaceRegion&) const = default;
};

struct AirportRunwayExit {
    std::string id;
    NodeId runway_node;
    EdgeId connector_edge;
    auto operator<=>(const AirportRunwayExit&) const = default;
};

struct AirportRunway {
    std::string id;
    std::string designation;
    Point2d threshold_09;
    Point2d threshold_27;
    double length_m{0.0};
    double width_m{0.0};
    std::string surface_type;
    NodeId departure_hold_node;
    std::vector<AirportRunwayExit> arrival_exits;
    auto operator<=>(const AirportRunway&) const = default;
};

struct AirportHoldPoint {
    std::string id;
    NodeId node;
    double protected_clearance_m{0.0};
    auto operator<=>(const AirportHoldPoint&) const = default;
};

struct AirportStandDesign {
    std::string id;
    GateId gate;
    NodeId node;
    std::vector<Point2d> footprint;
    double length_m{0.0};
    double width_m{0.0};
    double maximum_wingspan_m{0.0};
    double wingtip_clearance_m{0.0};
    std::string pushback_direction;
    bool tug_required{false};
    EdgeId aircraft_access_edge;
    EdgeId service_access_edge;
    auto operator<=>(const AirportStandDesign&) const = default;
};

struct AirportSupportArea {
    std::string id;
    std::string kind;
    NodeId reference_node;
    std::vector<Point2d> footprint;
    auto operator<=>(const AirportSupportArea&) const = default;
};

struct AirportDesign {
    std::string name;
    bool fictional{false};
    std::string coordinate_units{"meters"};
    std::string x_axis{"east"};
    std::string y_axis{"north"};
    std::string z_axis{"up"};
    NodeId origin_node;
    double unreal_centimeters_per_meter{100.0};
    std::vector<AirportRunway> runways;
    std::vector<AirportHoldPoint> hold_points;
    std::vector<AirportStandDesign> stands;
    std::vector<AirportSupportArea> support_areas;
    std::vector<AirportSurfaceRegion> regions;
    auto operator<=>(const AirportDesign&) const = default;
};

}  // namespace airside
