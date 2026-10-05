#include "airside/routing/astar.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <stdexcept>
#include <unordered_map>

namespace airside {
namespace {

double euclidean_distance(const Point2d& first, const Point2d& second) {
    return std::hypot(first.x_m - second.x_m, first.y_m - second.y_m);
}

double minimum_cost_per_meter(const AirportGraph& graph) {
    double result = std::numeric_limits<double>::infinity();
    for (const auto& edge : graph.edges()) {
        if (edge.available) {
            result = std::min(result, static_cast<double>(edge.traversal_cost.count()) / edge.distance_m);
        }
    }
    return std::isfinite(result) ? result : 0.0;
}

struct FrontierEntry {
    NodeId node;
    double estimated_total;
    std::int64_t cost;
};

struct LowerPriority {
    bool operator()(const FrontierEntry& left, const FrontierEntry& right) const noexcept {
        if (left.estimated_total != right.estimated_total) {
            return left.estimated_total > right.estimated_total;
        }
        if (left.cost != right.cost) {
            return left.cost > right.cost;
        }
        return left.node.value() > right.node.value();
    }
};

struct Predecessor {
    NodeId node;
    EdgeId edge;
};

}  // namespace

std::optional<Route> find_route(const AirportGraph& graph, NodeId origin, NodeId destination) {
    if (!graph.has_node(origin) || !graph.has_node(destination)) {
        throw std::invalid_argument("route endpoint is not in the graph");
    }
    if (origin == destination) {
        return Route{{origin}, {}, 0.0, SimTime::zero()};
    }

    const auto heuristic_scale = minimum_cost_per_meter(graph);
    const auto destination_position = graph.node(destination).position;
    const auto heuristic = [&](NodeId id) {
        return euclidean_distance(graph.node(id).position, destination_position) * heuristic_scale;
    };

    std::priority_queue<FrontierEntry, std::vector<FrontierEntry>, LowerPriority> frontier;
    std::unordered_map<NodeId, std::int64_t> costs;
    std::unordered_map<NodeId, Predecessor> predecessors;
    costs.emplace(origin, 0);
    frontier.push({origin, heuristic(origin), 0});

    while (!frontier.empty()) {
        const auto current = frontier.top();
        frontier.pop();
        if (current.cost != costs.at(current.node)) {
            continue;
        }
        if (current.node == destination) {
            break;
        }

        for (const auto edge_id : graph.adjacent_edges(current.node)) {
            const auto& edge = graph.edge(edge_id);
            if (!edge.available) {
                continue;
            }
            if (edge.one_way && edge.from != current.node) continue;
            const auto next = graph.other_endpoint(edge, current.node);
            const auto new_cost = current.cost + edge.traversal_cost.count();
            const auto known = costs.find(next);
            const bool improves = known == costs.end() || new_cost < known->second;
            const bool deterministic_tie = known != costs.end() && new_cost == known->second &&
                edge_id.value() < predecessors.at(next).edge.value();
            if (improves || deterministic_tie) {
                costs[next] = new_cost;
                predecessors[next] = {current.node, edge_id};
                frontier.push({next, static_cast<double>(new_cost) + heuristic(next), new_cost});
            }
        }
    }

    if (!costs.contains(destination)) {
        return std::nullopt;
    }

    Route result;
    result.travel_time = SimTime{costs.at(destination)};
    auto cursor = destination;
    result.nodes.push_back(cursor);
    while (cursor != origin) {
        const auto predecessor = predecessors.at(cursor);
        result.edges.push_back(predecessor.edge);
        result.distance_m += graph.edge(predecessor.edge).distance_m;
        cursor = predecessor.node;
        result.nodes.push_back(cursor);
    }
    std::ranges::reverse(result.nodes);
    std::ranges::reverse(result.edges);
    return result;
}

}  // namespace airside
