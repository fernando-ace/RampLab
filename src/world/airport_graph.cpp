#include "airside/world/airport_graph.hpp"

#include <algorithm>
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
