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

}  // namespace
}  // namespace airside
