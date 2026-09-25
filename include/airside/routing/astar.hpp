#pragma once

#include "airside/world/airport_graph.hpp"

#include <optional>
#include <vector>

namespace airside {

struct Route {
    std::vector<NodeId> nodes;
    std::vector<EdgeId> edges;
    double distance_m{0.0};
    SimTime travel_time{};

    constexpr auto operator<=>(const Route&) const = default;
};

[[nodiscard]] std::optional<Route> find_route(
    const AirportGraph& graph,
    NodeId origin,
    NodeId destination);

}  // namespace airside
