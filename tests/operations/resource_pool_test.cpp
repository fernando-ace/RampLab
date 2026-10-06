#include "airside/operations/resource_pool.hpp"

#include <gtest/gtest.h>

namespace airside {
namespace {

TEST(ResourcePoolTest, AssignsAvailableResourceImmediately) {
    ResourcePool pool{{VehicleId{2}, VehicleId{1}}};
    const auto result = pool.request(AircraftId{10});
    EXPECT_EQ(result.status, RequestStatus::Assigned);
    EXPECT_EQ(result.vehicle, VehicleId{1});
    EXPECT_EQ(pool.assigned_aircraft(VehicleId{1}), AircraftId{10});
}

TEST(ResourcePoolTest, QueuesRequestWhenResourceIsUnavailable) {
    ResourcePool pool{{VehicleId{1}}};
    EXPECT_EQ(pool.request(AircraftId{10}).status, RequestStatus::Assigned);
    EXPECT_EQ(pool.request(AircraftId{20}).status, RequestStatus::Queued);
    EXPECT_EQ(pool.waiting_count(), 1U);
}

TEST(ResourcePoolTest, AssignsOldestWaitingRequestOnRelease) {
    ResourcePool pool{{VehicleId{1}}};
    [[maybe_unused]] const auto first = pool.request(AircraftId{10});
    [[maybe_unused]] const auto second = pool.request(AircraftId{20});
    [[maybe_unused]] const auto third = pool.request(AircraftId{30});

    const auto next = pool.release(VehicleId{1});
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(next->aircraft, AircraftId{20});
    EXPECT_EQ(pool.waiting_count(), 1U);
    EXPECT_EQ(pool.available_count(), 0U);
}

TEST(ResourcePoolTest, ReturnsResourceToAvailablePoolAfterQueueDrains) {
    ResourcePool pool{{VehicleId{1}}};
    [[maybe_unused]] const auto assignment = pool.request(AircraftId{10});
    EXPECT_FALSE(pool.release(VehicleId{1}).has_value());
    EXPECT_EQ(pool.available_count(), 1U);
}

TEST(ResourcePoolTest, DuplicateRequestDoesNotEnterQueueTwice) {
    ResourcePool pool{{VehicleId{1}}};
    [[maybe_unused]] const auto first = pool.request(AircraftId{10});
    [[maybe_unused]] const auto second = pool.request(AircraftId{20});
    EXPECT_EQ(pool.request(AircraftId{20}).status, RequestStatus::AlreadyRequested);
    EXPECT_EQ(pool.waiting_count(), 1U);
}

TEST(ResourcePoolTest, DeadlinePriorityAndSimulationTimeAgingHaveStableTieBreaks) {
    ResourcePool pool{{VehicleId{1}}};
    [[maybe_unused]] const auto active = pool.request(AircraftId{10});
    [[maybe_unused]] const auto aged = pool.request(AircraftId{20}, 0, SimTime{0});
    [[maybe_unused]] const auto urgent = pool.request(AircraftId{30}, 8, SimTime{300});
    const auto urgent_next = pool.release(VehicleId{1}, SimTime{600});
    ASSERT_TRUE(urgent_next);
    EXPECT_EQ(urgent_next->aircraft, AircraftId{30});
    [[maybe_unused]] const auto later_urgent = pool.request(AircraftId{40}, 8, SimTime{600});
    const auto aged_next = pool.release(VehicleId{1}, SimTime{1100});
    ASSERT_TRUE(aged_next);
    EXPECT_EQ(aged_next->aircraft, AircraftId{20});
}

}  // namespace
}  // namespace airside
