#pragma once

#include "airside/core/types.hpp"

#include <span>
#include <string>
#include <vector>

namespace airside {

struct Point2d {
    double x_m{0.0};
    double y_m{0.0};
};

struct AirportNode {
    NodeId id;
    std::string name;
    Point2d position;
};

struct RoadEdge {
    EdgeId id;
    NodeId from;
    NodeId to;
    double distance_m{0.0};
    SimTime traversal_cost{};
    bool available{true};
};

class AirportGraph {
public:
    void add_node(AirportNode node);
    void add_edge(RoadEdge edge);

    [[nodiscard]] const AirportNode& node(NodeId id) const;
    [[nodiscard]] const RoadEdge& edge(EdgeId id) const;
    [[nodiscard]] std::span<const EdgeId> adjacent_edges(NodeId id) const;
    [[nodiscard]] NodeId other_endpoint(const RoadEdge& edge, NodeId endpoint) const;
    [[nodiscard]] bool has_node(NodeId id) const noexcept;

    void set_edge_available(EdgeId id, bool available);

    [[nodiscard]] std::span<const AirportNode> nodes() const noexcept;
    [[nodiscard]] std::span<const RoadEdge> edges() const noexcept;

private:
    struct Adjacency {
        NodeId node;
        std::vector<EdgeId> edges;
    };

    [[nodiscard]] Adjacency& adjacency(NodeId id);
    [[nodiscard]] const Adjacency& adjacency(NodeId id) const;

    std::vector<AirportNode> nodes_;
    std::vector<RoadEdge> edges_;
    std::vector<Adjacency> adjacency_;
};

}  // namespace airside
