#pragma once

#include "airside/world/airport_graph.hpp"

#include <optional>
#include <unordered_map>
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
    NodeId destination,
    MovementClass movement = MovementClass::Any);

// Non-negative per-edge expected waiting costs let adaptive policies evaluate
// congestion without changing graph availability or bypassing movement safety.
[[nodiscard]] std::optional<Route> find_route(
    const AirportGraph& graph,
    NodeId origin,
    NodeId destination,
    const std::unordered_map<EdgeId, SimTime>& expected_wait,
    MovementClass movement = MovementClass::Any);

// Enumerate a bounded set of simple, currently available routes in deterministic
// score order. This is for policy comparison only; movement still validates every
// selected route against authoritative reservations and closures.
[[nodiscard]] std::vector<Route> find_route_candidates(
    const AirportGraph& graph, NodeId origin, NodeId destination,
    std::size_t maximum_candidates,
    const std::unordered_map<EdgeId, SimTime>& expected_wait = {},
    MovementClass movement = MovementClass::Any);

}  // namespace airside
