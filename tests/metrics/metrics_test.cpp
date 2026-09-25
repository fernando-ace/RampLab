#include "airside/metrics/metrics.hpp"

#include <gtest/gtest.h>

#include <chrono>

namespace airside {
namespace {

using namespace std::chrono_literals;

TEST(MetricsTest, CalculatesTurnaroundDelayWaitingAndUtilization) {
    Aircraft flight{
        AircraftId{1}, "AX101", 0min, 20min, GateId{1}, NodeId{4},
        {{TaskId{1}, ServiceType::Fueling}, {TaskId{2}, ServiceType::Baggage}}};
    flight.arrive(0min);
    for (const auto type : {ServiceType::Fueling, ServiceType::Baggage}) {
        flight.mark_task_waiting(type, 0min);
        flight.assign_task(type);
        flight.start_task(type, 2min);
        flight.complete_task(type, type == ServiceType::Fueling ? 10min : 12min);
    }
    flight.depart(25min);

    ServiceVehicle fuel{VehicleId{1}, "FuelTruck-1", ServiceType::Fueling, NodeId{1}};
    const Route outbound{{NodeId{1}, NodeId{4}}, {EdgeId{1}}, 100.0, 1min};
    const Route inbound{{NodeId{4}, NodeId{1}}, {EdgeId{1}}, 100.0, 1min};
    fuel.assign(AircraftId{1}, outbound);
    fuel.arrive_at_aircraft(NodeId{4});
    fuel.start_service();
    fuel.finish_service(inbound, 8min);
    fuel.arrive_at_depot();

    const auto result = calculate_metrics({flight}, {fuel}, 30min);
    ASSERT_EQ(result.aircraft.size(), 1U);
    EXPECT_EQ(result.aircraft[0].turnaround, 25min);
    EXPECT_EQ(result.aircraft[0].departure_delay, 5min);
    EXPECT_EQ(result.aircraft[0].service_waiting, 4min);
    EXPECT_DOUBLE_EQ(result.fuel_utilization, 1.0 / 3.0);
    EXPECT_EQ(result.delayed_aircraft, 1U);
}

}  // namespace
}  // namespace airside
