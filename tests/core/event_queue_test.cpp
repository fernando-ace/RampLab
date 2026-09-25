#include "airside/core/event_queue.hpp"

#include <gtest/gtest.h>

#include <chrono>

namespace airside {
namespace {

using namespace std::chrono_literals;

TEST(EventQueueTest, ProcessesEventsChronologically) {
    EventQueue queue;
    [[maybe_unused]] const auto first = queue.schedule(30s, EventType::ServiceCompleted);
    [[maybe_unused]] const auto second = queue.schedule(10s, EventType::AircraftArrival);
    [[maybe_unused]] const auto third = queue.schedule(20s, EventType::VehicleArrivalAtAircraft);

    EXPECT_EQ(queue.pop()->timestamp, 10s);
    EXPECT_EQ(queue.pop()->timestamp, 20s);
    EXPECT_EQ(queue.pop()->timestamp, 30s);
}

TEST(EventQueueTest, PreservesInsertionOrderForTies) {
    EventQueue queue;
    const auto first = queue.schedule(10s, EventType::AircraftArrival, {EntityKind::Aircraft, 3});
    const auto second = queue.schedule(10s, EventType::AircraftArrival, {EntityKind::Aircraft, 1});

    EXPECT_EQ(queue.pop()->sequence, first);
    EXPECT_EQ(queue.pop()->sequence, second);
}

TEST(EventQueueTest, EmptyQueueIsSafe) {
    EventQueue queue;
    EXPECT_TRUE(queue.empty());
    EXPECT_EQ(queue.peek(), nullptr);
    EXPECT_FALSE(queue.pop().has_value());
}

TEST(EventQueueTest, EventsCanBeScheduledAfterProcessingBegins) {
    EventQueue queue;
    [[maybe_unused]] const auto initial = queue.schedule(5s, EventType::AircraftArrival);
    EXPECT_EQ(queue.pop()->timestamp, 5s);
    [[maybe_unused]] const auto follow_up = queue.schedule(8s, EventType::ServiceCompleted);
    EXPECT_EQ(queue.pop()->timestamp, 8s);
}

}  // namespace
}  // namespace airside
