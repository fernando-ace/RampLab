#include "airside/world/airport_graph.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <stdexcept>
#include <utility>

namespace airside {

void AirportGraph::add_node(AirportNode node_value) {
    if (has_node(node_value.id)) {
        throw std::invalid_argument("duplicate airport node ID");
    }
    nodes_.push_back(std::move(node_value));
    adjacency_.push_back(Adjacency{nodes_.back().id, {}});
}

void AirportGraph::add_edge(RoadEdge edge_value) {
    if (!has_node(edge_value.from) || !has_node(edge_value.to)) {
        throw std::invalid_argument("road edge references an unknown node");
    }
    if (edge_value.from == edge_value.to || edge_value.distance_m <= 0.0 ||
        edge_value.traversal_cost <= SimTime::zero()) {
        throw std::invalid_argument("road edge geometry and cost must be positive");
    }
    if (!edge_value.centerline.empty()) {
        if (edge_value.centerline.size() < 2 || std::ranges::any_of(edge_value.centerline, [](const Point2d& point) {
                return !std::isfinite(point.x_m) || !std::isfinite(point.y_m);
            }))
            throw std::invalid_argument("road edge centerline must contain at least two finite points");
        edge_value.centerline_length_m = 0.0;
        for (std::size_t i = 1; i < edge_value.centerline.size(); ++i)
            edge_value.centerline_length_m += std::hypot(edge_value.centerline[i].x_m - edge_value.centerline[i - 1].x_m,
                edge_value.centerline[i].y_m - edge_value.centerline[i - 1].y_m);
        if (edge_value.centerline_length_m <= 0.0)
            throw std::invalid_argument("road edge centerline length must be positive");
    }
    const auto duplicate = std::ranges::find_if(
        edges_, [&](const auto& existing) { return existing.id == edge_value.id; });
    if (duplicate != edges_.end()) {
        throw std::invalid_argument("duplicate road edge ID");
    }

    const auto id = edge_value.id;
    const auto from = edge_value.from;
    const auto to = edge_value.to;
    edges_.push_back(std::move(edge_value));
    adjacency(from).edges.push_back(id);
    adjacency(to).edges.push_back(id);
    std::ranges::sort(adjacency(from).edges);
    std::ranges::sort(adjacency(to).edges);
}

const AirportNode& AirportGraph::node(NodeId id) const {
    const auto found = std::ranges::find_if(nodes_, [&](const auto& value) { return value.id == id; });
    if (found == nodes_.end()) {
        throw std::out_of_range("unknown airport node ID");
    }
    return *found;
}

const RoadEdge& AirportGraph::edge(EdgeId id) const {
    const auto found = std::ranges::find_if(edges_, [&](const auto& value) { return value.id == id; });
    if (found == edges_.end()) {
        throw std::out_of_range("unknown road edge ID");
    }
    return *found;
}

std::span<const EdgeId> AirportGraph::adjacent_edges(NodeId id) const { return adjacency(id).edges; }

NodeId AirportGraph::other_endpoint(const RoadEdge& edge_value, NodeId endpoint) const {
    if (edge_value.from == endpoint) {
        return edge_value.to;
    }
    if (edge_value.to == endpoint) {
        return edge_value.from;
    }
    throw std::invalid_argument("node is not an endpoint of the road edge");
}

bool AirportGraph::has_node(NodeId id) const noexcept {
    return std::ranges::any_of(nodes_, [&](const auto& value) { return value.id == id; });
}

Point2d AirportGraph::edge_position(const RoadEdge& value, NodeId from, double fraction) const {
    if (from != value.from && from != value.to) throw std::invalid_argument("node is not an endpoint of the road edge");
    fraction = std::clamp(fraction, 0.0, 1.0);
    const std::array<Point2d, 2> fallback{node(value.from).position, node(value.to).position};
    const std::span<const Point2d> points = value.centerline.size() < 2
        ? std::span<const Point2d>{fallback} : std::span<const Point2d>{value.centerline};
    const bool reverse = from == value.to;
    const auto at = [&](std::size_t index) -> const Point2d& {
        return points[reverse ? points.size() - 1 - index : index];
    };
    const double total = value.centerline.size() >= 2 ? value.centerline_length_m :
        std::hypot(fallback[1].x_m - fallback[0].x_m, fallback[1].y_m - fallback[0].y_m);
    if (total <= 0.0) return at(0);
    double remaining = total * fraction;
    for (std::size_t i = 1; i < points.size(); ++i) {
        const auto dx = at(i).x_m - at(i - 1).x_m;
        const auto dy = at(i).y_m - at(i - 1).y_m;
        const auto length = std::hypot(dx, dy);
        if (remaining <= length || i + 1 == points.size()) {
            const auto t = length <= 0.0 ? 0.0 : std::clamp(remaining / length, 0.0, 1.0);
            return {at(i - 1).x_m + dx * t, at(i - 1).y_m + dy * t};
        }
        remaining -= length;
    }
    return at(points.size() - 1);
}

void AirportGraph::set_edge_available(EdgeId id, bool available) {
    const auto found = std::ranges::find_if(edges_, [&](const auto& value) { return value.id == id; });
    if (found == edges_.end()) {
        throw std::out_of_range("unknown road edge ID");
    }
    found->available = available;
}

std::span<const AirportNode> AirportGraph::nodes() const noexcept { return nodes_; }
std::span<const RoadEdge> AirportGraph::edges() const noexcept { return edges_; }

AirportGraph::Adjacency& AirportGraph::adjacency(NodeId id) {
    return const_cast<Adjacency&>(std::as_const(*this).adjacency(id));
}

const AirportGraph::Adjacency& AirportGraph::adjacency(NodeId id) const {
    const auto found = std::ranges::find_if(
        adjacency_, [&](const auto& value) { return value.node == id; });
    if (found == adjacency_.end()) {
        throw std::out_of_range("unknown airport node ID");
    }
    return *found;
}

}  // namespace airside
