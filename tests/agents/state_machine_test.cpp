#include "airside/agents/aircraft.hpp"
#include "airside/agents/service_vehicle.hpp"

#include <gtest/gtest.h>

#include <chrono>

namespace airside {
namespace {

using namespace std::chrono_literals;

Aircraft make_aircraft() {
    return Aircraft{
        AircraftId{1}, "AX101", 0s, 30min, GateId{1}, NodeId{4},
        {{TaskId{1}, ServiceType::Fueling}, {TaskId{2}, ServiceType::Baggage}}};
}

TEST(AircraftStateTest, FollowsExplicitValidTransitions) {
    auto aircraft = make_aircraft();
    aircraft.arrive(0s);
    EXPECT_EQ(aircraft.state(), AircraftState::WaitingForServices);
    aircraft.assign_task(ServiceType::Fueling);
    aircraft.start_task(ServiceType::Fueling, 1min);
    aircraft.complete_task(ServiceType::Fueling, 9min);
    aircraft.assign_task(ServiceType::Baggage);
    aircraft.start_task(ServiceType::Baggage, 1min);
    aircraft.complete_task(ServiceType::Baggage, 11min);
    EXPECT_EQ(aircraft.state(), AircraftState::ReadyForPushback);
    aircraft.depart(30min);
    EXPECT_EQ(aircraft.state(), AircraftState::Departed);
}

TEST(AircraftStateTest, RejectsInvalidTransitionAndPrematureDeparture) {
    auto aircraft = make_aircraft();
    EXPECT_THROW(aircraft.transition_to(AircraftState::Departed), std::logic_error);
    aircraft.arrive(0s);
    EXPECT_THROW(aircraft.depart(5min), std::logic_error);
}

TEST(AircraftStateTest, BecomesReadyOnlyAfterEveryRequiredService) {
    auto aircraft = make_aircraft();
    aircraft.arrive(0s);
    for (const auto type : {ServiceType::Fueling, ServiceType::Baggage}) {
        aircraft.assign_task(type);
        aircraft.start_task(type, 1min);
        aircraft.complete_task(type, type == ServiceType::Fueling ? 9min : 11min);
        if (type == ServiceType::Fueling) {
            EXPECT_EQ(aircraft.state(), AircraftState::WaitingForServices);
        }
    }
    EXPECT_TRUE(aircraft.services_complete());
    EXPECT_EQ(aircraft.ready_at(), 11min);
}

TEST(VehicleStateTest, ExecutesAssignmentServiceAndReturnLifecycle) {
    ServiceVehicle vehicle{VehicleId{1}, "FuelTruck-1", ServiceType::Fueling, NodeId{1}};
    const Route outbound{{NodeId{1}, NodeId{4}}, {EdgeId{1}}, 100.0, 1min};
    const Route inbound{{NodeId{4}, NodeId{1}}, {EdgeId{1}}, 100.0, 1min};
    vehicle.assign(AircraftId{1}, outbound, 5min);
    EXPECT_EQ(vehicle.state(), VehicleState::TravelingToAircraft);
    vehicle.arrive_at_aircraft(NodeId{4});
    vehicle.start_service();
    vehicle.finish_service(inbound, 8min, 14min);
    vehicle.arrive_at_depot();
    EXPECT_EQ(vehicle.state(), VehicleState::Idle);
    EXPECT_EQ(vehicle.busy_time(), 10min);
}

TEST(VehicleStateTest, RejectsStartingServiceWhileIdle) {
    ServiceVehicle vehicle{VehicleId{1}, "FuelTruck-1", ServiceType::Fueling, NodeId{1}};
    EXPECT_THROW(vehicle.start_service(), std::logic_error);
}

}  // namespace
}  // namespace airside
