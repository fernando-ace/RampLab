#include "support/scenarios.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>

namespace airside {
namespace {

using namespace std::chrono_literals;

class CapturingSink final : public ISimulationEventSink {
public:
    void on_event(const SimulationEventRecord& event) noexcept override { events.push_back(event); }
    std::vector<SimulationEventRecord> events;
};

TEST(EventStreamTest, EmitsOrderedTypedLifecycleRecordsWithEntityIds) {
    Simulation simulation{test::baseline_scenario(), 42};
    CapturingSink sink;
    simulation.add_event_sink(sink);
    const auto result = simulation.run();

    ASSERT_EQ(sink.events, result.events);
    ASSERT_FALSE(result.events.empty());
    for (std::size_t index = 0; index < result.events.size(); ++index) {
        EXPECT_EQ(result.events[index].sequence, index);
        if (index > 0) EXPECT_LE(result.events[index - 1].timestamp, result.events[index].timestamp);
    }

    const auto wait = std::ranges::find_if(result.events, [](const auto& event) {
        return event.type == SimulationEventType::ResourceWaitStarted &&
            event.aircraft == AircraftId{2} && event.service == ServiceType::Fueling;
    });
    ASSERT_NE(wait, result.events.end());
    EXPECT_EQ(wait->timestamp, 4min);

    EXPECT_TRUE(std::ranges::any_of(result.events, [](const auto& event) {
        return event.type == SimulationEventType::ServiceStarted && event.vehicle.has_value() &&
            event.aircraft.has_value() && event.service.has_value();
    }));
    EXPECT_TRUE(std::ranges::any_of(result.events, [](const auto& event) {
        return event.type == SimulationEventType::ServiceCompleted;
    }));
    EXPECT_TRUE(std::ranges::any_of(result.events, [](const auto& event) {
        return event.type == SimulationEventType::RoadClosed && event.edge == EdgeId{4} &&
            event.timestamp == 5min;
    }));
    EXPECT_TRUE(std::ranges::any_of(result.events, [](const auto& event) {
        return event.type == SimulationEventType::AircraftDeparted &&
            event.aircraft == AircraftId{3} && event.timestamp == 47min;
    }));
}

TEST(EventStreamTest, RegisteringMultipleSinksDoesNotChangeDomainResult) {
    const auto without_sink = Simulation{test::baseline_scenario(), 42}.run();
    Simulation observed{test::baseline_scenario(), 42};
    CapturingSink first;
    CapturingSink second;
    observed.add_event_sink(first);
    observed.add_event_sink(second);
    const auto with_sinks = observed.run();

    EXPECT_EQ(with_sinks.events, without_sink.events);
    EXPECT_EQ(with_sinks.metrics, without_sink.metrics);
    EXPECT_EQ(first.events, second.events);
}

}  // namespace
}  // namespace airside
