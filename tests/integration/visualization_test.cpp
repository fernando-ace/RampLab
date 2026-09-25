#include "airside/integration/visualization.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <vector>

namespace airside::visualization {
namespace {

using namespace std::chrono_literals;

TEST(VisualizationTest, ConvertsMetersWithOneCentralTransform) {
    const CoordinateTransform transform{100.0, {500.0, -250.0}, true};
    const auto result = transform.apply({2.5, -3.0});
    EXPECT_DOUBLE_EQ(result.x, 750.0);
    EXPECT_DOUBLE_EQ(result.y, 50.0);
}

TEST(VisualizationTest, InterpolatesAcrossCompleteMultiSegmentJourney) {
    const std::vector<RoadNodeSnapshot> nodes{
        {NodeId{1}, "origin", {0.0, 0.0}},
        {NodeId{2}, "corner", {10.0, 0.0}},
        {NodeId{3}, "destination", {10.0, 20.0}},
    };
    const VehicleJourneySnapshot journey{
        NodeId{1}, NodeId{3}, 10s, 40s,
        {NodeId{1}, NodeId{2}, NodeId{3}},
        {EdgeId{1}, EdgeId{2}},
        {
            {EdgeId{1}, NodeId{1}, NodeId{2}, 10s, 20s, 10.0},
            {EdgeId{2}, NodeId{2}, NodeId{3}, 20s, 40s, 20.0},
        },
    };

    const auto first = sample_journey(journey, nodes, 15s);
    ASSERT_TRUE(first);
    EXPECT_EQ(first->segment_index, 0U);
    EXPECT_DOUBLE_EQ(first->segment_progress, 0.5);
    EXPECT_DOUBLE_EQ(first->position_m.x, 5.0);
    EXPECT_DOUBLE_EQ(first->position_m.y, 0.0);
    EXPECT_DOUBLE_EQ(first->direction.x, 1.0);
    EXPECT_DOUBLE_EQ(first->direction.y, 0.0);

    const auto second = sample_journey(journey, nodes, 30s);
    ASSERT_TRUE(second);
    EXPECT_EQ(second->segment_index, 1U);
    EXPECT_DOUBLE_EQ(second->segment_progress, 0.5);
    EXPECT_DOUBLE_EQ(second->position_m.x, 10.0);
    EXPECT_DOUBLE_EQ(second->position_m.y, 10.0);
    EXPECT_DOUBLE_EQ(second->direction.x, 0.0);
    EXPECT_DOUBLE_EQ(second->direction.y, 1.0);
}

TEST(VisualizationTest, ClampsBeforeDepartureAndAtExactDestination) {
    const std::vector<RoadNodeSnapshot> nodes{
        {NodeId{1}, "origin", {2.0, 3.0}},
        {NodeId{2}, "destination", {8.0, 9.0}},
    };
    const VehicleJourneySnapshot journey{
        NodeId{1}, NodeId{2}, 10s, 20s,
        {NodeId{1}, NodeId{2}}, {EdgeId{1}},
        {{EdgeId{1}, NodeId{1}, NodeId{2}, 10s, 20s, 10.0}},
    };

    const auto before = sample_journey(journey, nodes, 5s);
    const auto after = sample_journey(journey, nodes, 25s);
    ASSERT_TRUE(before);
    ASSERT_TRUE(after);
    EXPECT_EQ(before->position_m, (Point2{2.0, 3.0}));
    EXPECT_EQ(after->position_m, (Point2{8.0, 9.0}));
    EXPECT_DOUBLE_EQ(after->segment_progress, 1.0);
}

TEST(VisualizationTest, RejectsMissingOrMalformedJourneyData) {
    VehicleJourneySnapshot empty{};
    EXPECT_FALSE(sample_journey(empty, {}, SimTime::zero()));

    empty.segments.push_back({EdgeId{1}, NodeId{1}, NodeId{2}, 0s, 1s, 1.0});
    EXPECT_FALSE(sample_journey(empty, {}, SimTime::zero()));
}

}  // namespace
}  // namespace airside::visualization
