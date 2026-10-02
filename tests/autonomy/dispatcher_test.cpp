#include "airside/autonomy/dispatcher.hpp"

#include <gtest/gtest.h>

#include <algorithm>

namespace airside::autonomy {
namespace {
AirportGraph graph() {
    AirportGraph result;
    result.add_node({NodeId{1}, "Depot", {0.0, 0.0}});
    result.add_node({NodeId{2}, "Junction", {20.0, 0.0}});
    result.add_node({NodeId{3}, "Gate", {30.0, 0.0}});
    result.add_node({NodeId{4}, "Remote", {0.0, 40.0}});
    result.add_edge({EdgeId{1}, NodeId{1}, NodeId{2}, 20.0, std::chrono::seconds{20}, true});
    result.add_edge({EdgeId{2}, NodeId{2}, NodeId{3}, 10.0, std::chrono::seconds{10}, true});
    result.add_edge({EdgeId{3}, NodeId{1}, NodeId{4}, 40.0, std::chrono::seconds{40}, true});
    return result;
}
ServiceRequest task(std::string id, int priority = 0, double release = 0.0, std::string capability = "tug") {
    return {ServiceRequestId{std::move(id)}, ServiceKind::TugCartMovement, std::move(capability),
            "Junction", "Gate", release, priority, std::nullopt, 0.0};
}
DispatchVehicle vehicle(std::string id, std::string node, std::vector<std::string> capabilities = {"tug"}) {
    return {VehicleId{std::move(id)}, std::move(capabilities), std::move(node), DispatchVehicleState::Idle, std::nullopt};
}
}

TEST(FleetDispatcherTest, ChoosesLowestAvailableRouteCostAndUsesStableIdForExactTies) {
    FleetDispatcher dispatcher{{task("job")}};
    const auto selected = dispatcher.dispatch(0.0, {vehicle("far", "Depot"), vehicle("near_b", "Junction"), vehicle("near_a", "Junction")}, graph());
    ASSERT_EQ(selected.size(), 1U);
    EXPECT_EQ(selected.front().second.value, "near_a");
    const auto events = dispatcher.events();
    const auto assigned = std::ranges::find(events, DispatchEventKind::Assigned, &DispatchEvent::kind);
    ASSERT_NE(assigned, events.end());
    EXPECT_DOUBLE_EQ(assigned->route_distance_m, 0.0);
    EXPECT_NE(assigned->detail.find("stable_vehicle_id_tie_break"), std::string::npos);
}

TEST(FleetDispatcherTest, CapabilityFilteringPrecedesDistance) {
    FleetDispatcher dispatcher{{task("fuel_job", 0, 0.0, "fuel")}};
    const auto selected = dispatcher.dispatch(0.0, {vehicle("near_tug", "Junction"), vehicle("fuel_01", "Depot", {"fuel"})}, graph());
    ASSERT_EQ(selected.size(), 1U);
    EXPECT_EQ(selected.front().second.value, "fuel_01");
    EXPECT_TRUE(std::ranges::any_of(dispatcher.events(), [](const auto& event) {
        return event.kind == DispatchEventKind::CandidateEvaluated && event.vehicle.value == "near_tug" &&
               event.detail == "incompatible:fuel";
    }));
}

TEST(FleetDispatcherTest, RequestsReleaseAtSimulationTimeAndAssignmentsAreDeterministic) {
    FleetDispatcher a{{task("early", 0, 0.0), task("later", 0, 3.0)}};
    FleetDispatcher b{{task("later", 0, 3.0), task("early", 0, 0.0)}};
    const auto fleet = std::vector<DispatchVehicle>{vehicle("tug_01", "Depot"), vehicle("tug_02", "Remote")};
    EXPECT_EQ(a.next_release_time_s(), 0.0);
    const auto first = a.dispatch(0.0, fleet, graph());
    ASSERT_EQ(first.size(), 1U);
    EXPECT_EQ(b.dispatch(0.0, fleet, graph()).size(), 1U);
    EXPECT_EQ(a.snapshot()[1].state, ServiceTaskState::Queued);
    const auto later = a.dispatch(3.0, fleet, graph());
    ASSERT_EQ(later.size(), 1U);
    EXPECT_NE(later.front().second, first.front().second);
    EXPECT_EQ(b.dispatch(3.0, fleet, graph()).size(), 1U);
    EXPECT_FALSE(a.next_release_time_s());
    EXPECT_EQ(a.events(), b.events());
    EXPECT_EQ(a.snapshot()[1].state, ServiceTaskState::Assigned);
}

TEST(FleetDispatcherTest, PrioritySelectsWorkFirstAndAgingEventuallyPromotesOldRequests) {
    FleetDispatcher dispatcher{{task("aged", 0, 0.0), task("priority_0", 2, 0.0),
                                task("priority_1", 2, 1.0), task("priority_2", 2, 2.0)}, 1.0};
    const auto finish = [&](const ServiceRequestId& id, double t) {
        dispatcher.set_state(id, ServiceTaskState::EnRoute, t + 0.1);
        dispatcher.set_state(id, ServiceTaskState::Servicing, t + 0.2);
        dispatcher.set_state(id, ServiceTaskState::Completed, t + 0.3);
    };
    auto first = dispatcher.dispatch(0.0, {vehicle("tug", "Junction")}, graph());
    ASSERT_EQ(first.size(), 1U);
    EXPECT_EQ(first.front().first.value, "priority_0");
    finish(first.front().first, 0.0);
    auto second = dispatcher.dispatch(1.0, {vehicle("tug", "Junction")}, graph());
    ASSERT_EQ(second.size(), 1U);
    EXPECT_EQ(second.front().first.value, "priority_1");
    finish(second.front().first, 1.0);
    auto third = dispatcher.dispatch(2.0, {vehicle("tug", "Junction")}, graph());
    ASSERT_EQ(third.size(), 1U);
    EXPECT_EQ(third.front().first.value, "aged");
    finish(third.front().first, 2.0);
    auto fourth = dispatcher.dispatch(3.0, {vehicle("tug", "Junction")}, graph());
    ASSERT_EQ(fourth.size(), 1U);
    EXPECT_EQ(fourth.front().first.value, "priority_2");
    finish(fourth.front().first, 3.0);
    EXPECT_GE(dispatcher.metrics().aging_activations, 2U);
    EXPECT_TRUE(dispatcher.all_terminal());
    EXPECT_TRUE(std::ranges::any_of(dispatcher.events(), [](const auto& event) {
        return event.kind == DispatchEventKind::AgingApplied && event.request.value == "aged";
    }));
}

TEST(FleetDispatcherTest, VehicleUnavailabilityRequeuesAndReassignsWithoutChangingProgress) {
    FleetDispatcher dispatcher{{task("handoff")}};
    ASSERT_EQ(dispatcher.dispatch(0.0, {vehicle("tug_a", "Junction"), vehicle("tug_b", "Depot")}, graph()).size(), 1U);
    dispatcher.set_state(ServiceRequestId{"handoff"}, ServiceTaskState::EnRoute, 1.0);
    dispatcher.mark_vehicle_unavailable(VehicleId{"tug_a"}, 2.0);
    EXPECT_EQ(dispatcher.snapshot().front().state, ServiceTaskState::Queued);
    auto failed_vehicle = vehicle("tug_a", "Junction");
    failed_vehicle.state = DispatchVehicleState::Unavailable;
    const auto assigned = dispatcher.dispatch(2.0, {failed_vehicle, vehicle("tug_b", "Depot")}, graph());
    ASSERT_EQ(assigned.size(), 1U);
    EXPECT_EQ(assigned.front().second.value, "tug_b");
    EXPECT_EQ(dispatcher.snapshot().front().reassignments, 1U);
    EXPECT_EQ(dispatcher.metrics().reassignments, 1U);
    EXPECT_TRUE(std::ranges::any_of(dispatcher.events(), [](const auto& event) { return event.kind == DispatchEventKind::Reassigned; }));
}

TEST(FleetDispatcherTest, BusyDegradedUnavailableAndRecoveringVehiclesAreNotCandidates) {
    FleetDispatcher dispatcher{{task("job")}};
    auto unavailable = vehicle("u", "Junction"); unavailable.state = DispatchVehicleState::Unavailable;
    auto recovering = vehicle("r", "Junction"); recovering.state = DispatchVehicleState::Recovering;
    auto degraded = vehicle("d", "Junction"); degraded.state = DispatchVehicleState::Degraded;
    auto busy = vehicle("b", "Junction"); busy.state = DispatchVehicleState::Busy;
    EXPECT_TRUE(dispatcher.dispatch(0.0, {unavailable, recovering, degraded, busy}, graph()).empty());
    EXPECT_EQ(dispatcher.snapshot().front().state, ServiceTaskState::Queued);
}

TEST(FleetDispatcherTest, ClosedRoadMakesCandidateRouteUnavailableAndTerminalStatesAreValidated) {
    auto roads = graph();
    roads.set_edge_available(EdgeId{2}, false);
    FleetDispatcher dispatcher{{task("job")}};
    EXPECT_TRUE(dispatcher.dispatch(0.0, {vehicle("tug", "Junction")}, roads).empty());
    EXPECT_TRUE(std::ranges::any_of(dispatcher.events(), [](const auto& event) { return event.detail == "no_available_task_route"; }));
    EXPECT_THROW(dispatcher.set_state(ServiceRequestId{"job"}, ServiceTaskState::Completed, 1.0), std::invalid_argument);
    dispatcher.fail(ServiceRequestId{"job"}, 1.0, "no_compatible_vehicle");
    EXPECT_TRUE(dispatcher.all_terminal());
    EXPECT_EQ(dispatcher.metrics().requests_failed, 1U);
}

} // namespace airside::autonomy
