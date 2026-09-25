#include "airside/routing/astar.hpp"

#include <gtest/gtest.h>

#include <chrono>

namespace airside {
namespace {

using namespace std::chrono_literals;

AirportGraph make_graph() {
    AirportGraph graph;
    graph.add_node({NodeId{1}, "Depot", {0.0, 0.0}});
    graph.add_node({NodeId{2}, "Junction North", {100.0, 100.0}});
    graph.add_node({NodeId{3}, "Junction South", {100.0, -100.0}});
    graph.add_node({NodeId{4}, "Gate", {200.0, 0.0}});
    graph.add_edge({EdgeId{1}, NodeId{1}, NodeId{2}, 141.0, 10s, true});
    graph.add_edge({EdgeId{2}, NodeId{2}, NodeId{4}, 141.0, 10s, true});
    graph.add_edge({EdgeId{3}, NodeId{1}, NodeId{3}, 141.0, 12s, true});
    graph.add_edge({EdgeId{4}, NodeId{3}, NodeId{4}, 141.0, 12s, true});
    graph.add_edge({EdgeId{5}, NodeId{1}, NodeId{4}, 300.0, 30s, true});
    return graph;
}

TEST(AStarTest, FindsShortestAvailableRoute) {
    const auto graph = make_graph();
    const auto route = find_route(graph, NodeId{1}, NodeId{4});
    ASSERT_TRUE(route.has_value());
    EXPECT_EQ(route->nodes, (std::vector{NodeId{1}, NodeId{2}, NodeId{4}}));
    EXPECT_EQ(route->travel_time, 20s);
}

TEST(AStarTest, DisabledEdgeCausesReroute) {
    auto graph = make_graph();
    graph.set_edge_available(EdgeId{2}, false);
    const auto route = find_route(graph, NodeId{1}, NodeId{4});
    ASSERT_TRUE(route.has_value());
    EXPECT_EQ(route->nodes, (std::vector{NodeId{1}, NodeId{3}, NodeId{4}}));
    EXPECT_EQ(route->travel_time, 24s);
}

TEST(AStarTest, ReportsUnreachableDestination) {
    auto graph = make_graph();
    graph.set_edge_available(EdgeId{2}, false);
    graph.set_edge_available(EdgeId{4}, false);
    graph.set_edge_available(EdgeId{5}, false);
    EXPECT_FALSE(find_route(graph, NodeId{1}, NodeId{4}).has_value());
}

TEST(AStarTest, EqualCostPathSelectionIsDeterministic) {
    auto graph = make_graph();
    graph.set_edge_available(EdgeId{5}, false);
    graph.set_edge_available(EdgeId{3}, false);
    graph.add_edge({EdgeId{6}, NodeId{1}, NodeId{3}, 141.0, 10s, true});
    graph.set_edge_available(EdgeId{4}, false);
    graph.add_edge({EdgeId{7}, NodeId{3}, NodeId{4}, 141.0, 10s, true});

    const auto first = find_route(graph, NodeId{1}, NodeId{4});
    const auto second = find_route(graph, NodeId{1}, NodeId{4});
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first, second);
    EXPECT_EQ(first->nodes, (std::vector{NodeId{1}, NodeId{2}, NodeId{4}}));
}

}  // namespace
}  // namespace airside
