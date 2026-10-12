#include "airside/agents/operational_agent.hpp"

#include <gtest/gtest.h>

namespace airside::agents {
namespace {

Route route(std::initializer_list<std::uint32_t> nodes) {
    Route result;
    for (const auto node : nodes) result.nodes.emplace_back(node);
    for (std::size_t index = 1; index < result.nodes.size(); ++index)
        result.edges.emplace_back(static_cast<std::uint32_t>(index));
    return result;
}

Observation base_observation() {
    Observation value;
    value.agent_id = "aircraft-7";
    value.current_node = NodeId{1};
    value.destination = NodeId{4};
    value.current_route = route({1, 2, 4});
    value.available_route = route({1, 3, 4});
    value.current_route_cost = SimTime{100};
    value.available_route_cost = SimTime{80};
    return value;
}

Objective destination_objective() {
    return {"aircraft-7-departure", ObjectiveType::DepartAirport, NodeId{4}, 10};
}

TEST(OperationalAgentPolicyTests, ScriptedPolicyReplansOnInvalidationAndKeepsSafeNoRouteBlocked) {
    ScriptedRoutingPolicy policy;
    auto observation = base_observation();
    observation.route_invalidated = true;
    observation.invalid_edge = EdgeId{5};
    const auto decision = policy.decide(observation, destination_objective(), {});
    EXPECT_EQ(decision.action, ActionType::PlanRoute);
    EXPECT_EQ(decision.reason, ReasonCode::RouteClosed);
    ASSERT_TRUE(decision.route);
    EXPECT_EQ(route_string(decision.route), "1->3->4");

    observation.available_route.reset();
    const auto blocked = policy.decide(observation, destination_objective(), {});
    EXPECT_EQ(blocked.action, ActionType::FailObjective);
    EXPECT_EQ(blocked.status, AgentStatus::Failed);
    EXPECT_EQ(blocked.reason, ReasonCode::NoSafeRoute);
}

TEST(OperationalAgentPolicyTests, AdaptivePolicyUsesThresholdAndCooldownBeforeCongestionReplan) {
    AdaptiveRoutingPolicy policy{SimTime{15}, SimTime{10}};
    auto observation = base_observation();
    observation.reservation_blocked = true;
    observation.blocked_for = SimTime{40};
    observation.time = SimTime{20};
    PolicyMemory memory;
    memory.last_destination = NodeId{4};
    memory.last_replan_at = SimTime{15};

    auto decision = policy.decide(observation, destination_objective(), memory);
    EXPECT_EQ(decision.action, ActionType::Wait);
    EXPECT_EQ(decision.reason, ReasonCode::ReservationBlocked);

    observation.time = SimTime{26};
    observation.available_route_cost = SimTime{70};
    decision = policy.decide(observation, destination_objective(), memory);
    EXPECT_EQ(decision.action, ActionType::PlanRoute);
    EXPECT_EQ(decision.reason, ReasonCode::Congestion);
}

TEST(OperationalAgentPolicyTests, ClosureReplanIsImmediateAndPolicyOutputIsDeterministic) {
    AdaptiveRoutingPolicy policy;
    auto observation = base_observation();
    observation.route_invalidated = true;
    observation.invalid_edge = EdgeId{8};
    const auto first = policy.decide(observation, destination_objective(), {});
    const auto second = policy.decide(observation, destination_objective(), {});
    EXPECT_EQ(first, second);
    EXPECT_EQ(first.action, ActionType::PlanRoute);
    EXPECT_EQ(first.reason, ReasonCode::RouteClosed);
}

TEST(OperationalAgentPolicyTests, ObjectiveCompletionAndDestinationChangesAreExplicit) {
    ScriptedRoutingPolicy policy;
    auto observation = base_observation();
    observation.objective_satisfied = true;
    auto decision = policy.decide(observation, destination_objective(), {});
    EXPECT_EQ(decision.action, ActionType::CompleteObjective);
    EXPECT_EQ(decision.reason, ReasonCode::ObjectiveCompleted);

    observation.objective_satisfied = false;
    PolicyMemory memory;
    memory.last_destination = NodeId{3};
    decision = policy.decide(observation, destination_objective(), memory);
    EXPECT_EQ(decision.action, ActionType::PlanRoute);
    EXPECT_EQ(decision.reason, ReasonCode::DestinationChanged);
}

TEST(OperationalAgentPolicyTests, AutonomyModeParsingIsStableAndRejectsUnknownModes) {
    EXPECT_EQ(parse_autonomy_mode("SCRIPTED"), AutonomyMode::Scripted);
    EXPECT_EQ(parse_autonomy_mode("ADAPTIVE"), AutonomyMode::Adaptive);
    EXPECT_EQ(to_string(AutonomyMode::Adaptive), "ADAPTIVE");
    EXPECT_THROW((void)parse_autonomy_mode("RANDOM"), std::invalid_argument);
}

}  // namespace
}  // namespace airside::agents
