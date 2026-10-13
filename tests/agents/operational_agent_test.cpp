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

TEST(OperationalAgentPolicyTests, AdaptivePolicyChoosesLowerScoringRouteBeforeAClosureOrBlockage) {
    AdaptiveRoutingPolicy policy{SimTime{15}, SimTime{0}};
    auto observation = base_observation();
    observation.candidates = {
        {CandidateKind::ContinueCurrentRoute, route({1, 2, 4}), 220.0, SimTime{100}, SimTime{100}, 220.0, true, "busy"},
        {CandidateKind::AlternateRoute, route({1, 3, 4}), 140.0, SimTime{80}, SimTime{0}, 140.0, true, "clear"}
    };
    const auto decision = policy.decide(observation, destination_objective(), {});
    EXPECT_EQ(decision.action, ActionType::PlanRoute);
    EXPECT_EQ(decision.route, observation.candidates[1].route);
    EXPECT_EQ(decision.reason, ReasonCode::Congestion);
}

TEST(OperationalAgentPolicyTests, WaitCandidateIncludesContinuationTaxiCost) {
    AdaptiveRoutingPolicy policy{SimTime{15}, SimTime{0}};
    auto observation = base_observation();
    observation.reservation_blocked = true;
    observation.candidates = {
        {CandidateKind::ContinueCurrentRoute, route({1, 2, 4}), 100.0, SimTime{80}, SimTime{60}, 142.0, true, "current"},
        {CandidateKind::AlternateRoute, route({1, 3, 4}), 100.0, SimTime{100}, SimTime{0}, 102.0, true, "alternate"},
        {CandidateKind::Wait, route({1, 2, 4}), 100.0, SimTime{80}, SimTime{60}, 142.0, true, "wait then continue"}
    };
    const auto decision = policy.decide(observation, destination_objective(), {});
    EXPECT_EQ(decision.action, ActionType::PlanRoute);
    EXPECT_EQ(decision.route, observation.candidates[1].route);
    EXPECT_EQ(decision.reason, ReasonCode::Congestion);
}

TEST(OperationalAgentPolicyTests, AdaptiveCandidateTieBreakUsesStableCandidateOrder) {
    AdaptiveRoutingPolicy policy{SimTime{0}, SimTime{0}};
    auto observation = base_observation();
    observation.current_route.reset();
    observation.candidates = {
        {CandidateKind::AlternateRoute, route({1, 2, 4}), 100.0, SimTime{80}, SimTime{0}, 80.0, true, "first"},
        {CandidateKind::AlternateRoute, route({1, 3, 4}), 100.0, SimTime{80}, SimTime{0}, 80.0, true, "second"}
    };
    const auto first = policy.decide(observation, destination_objective(), {});
    const auto replay = policy.decide(observation, destination_objective(), {});
    EXPECT_EQ(first, replay);
    EXPECT_EQ(first.route, observation.candidates.front().route);
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
    EXPECT_EQ(parse_autonomy_mode("COORDINATED_ADAPTIVE"), AutonomyMode::CoordinatedAdaptive);
    EXPECT_EQ(to_string(AutonomyMode::CoordinatedAdaptive), "COORDINATED_ADAPTIVE");
    EXPECT_EQ(parse_autonomy_mode("ROLLING_HORIZON_COORDINATED"), AutonomyMode::RollingHorizonCoordinated);
    EXPECT_EQ(to_string(AutonomyMode::RollingHorizonCoordinated), "ROLLING_HORIZON_COORDINATED");
    EXPECT_EQ(parse_autonomy_mode("COUNTERFACTUAL"), AutonomyMode::Counterfactual);
    EXPECT_EQ(to_string(AutonomyMode::Counterfactual), "COUNTERFACTUAL");
    EXPECT_EQ(parse_autonomy_mode("COST_AWARE_COUNTERFACTUAL"), AutonomyMode::CostAwareCounterfactual);
    EXPECT_EQ(to_string(AutonomyMode::CostAwareCounterfactual), "COST_AWARE_COUNTERFACTUAL");
    EXPECT_THROW((void)parse_autonomy_mode("RANDOM"), std::invalid_argument);
}

TEST(OperationalAgentPolicyTests, CostAwareCounterfactualRejectsWastefulDetourAndAcceptsWorthwhileOne) {
    CostAwareCounterfactualPolicy policy;
    auto objective = destination_objective();
    auto make_observation = [&] {
        auto observation = base_observation();
        observation.current_route = route({1, 2, 4});
        CandidateEvaluation baseline;
        baseline.kind = CandidateKind::ContinueCurrentRoute;
        baseline.route = observation.current_route;
        baseline.counterfactual_evaluated = true;
        baseline.rollout_completed_objectives = 1U;
        baseline.rollout_wait = SimTime{2};
        baseline.rollout_taxi_time = SimTime{100};
        baseline.rollout_completion_time = SimTime{102};
        baseline.rollout_distance_m = 500.0;
        CandidateEvaluation alternative;
        alternative.kind = CandidateKind::AlternateRoute;
        alternative.route = route({1, 3, 4});
        alternative.counterfactual_evaluated = true;
        alternative.rollout_completed_objectives = 1U;
        observation.candidates = {baseline, alternative};
        return observation;
    };

    auto wasteful = make_observation();
    wasteful.candidates[1].rollout_wait = SimTime{0};
    wasteful.candidates[1].rollout_taxi_time = SimTime{190};
    wasteful.candidates[1].rollout_completion_time = SimTime{190};
    wasteful.candidates[1].rollout_distance_m = 900.0;
    evaluate_operational_costs(wasteful, objective);
    EXPECT_FALSE(wasteful.candidates[1].material_improvement_passed);
    EXPECT_FALSE(wasteful.candidates[1].bounded_regret_passed);
    auto decision = policy.decide(wasteful, objective, {});
    ASSERT_EQ(decision.action, ActionType::FollowRoute);
    EXPECT_EQ(decision.route, wasteful.current_route);

    auto worthwhile = make_observation();
    worthwhile.candidates[0].rollout_wait = SimTime{90};
    worthwhile.candidates[0].rollout_completion_time = SimTime{190};
    worthwhile.candidates[1].rollout_wait = SimTime{0};
    worthwhile.candidates[1].rollout_taxi_time = SimTime{108};
    worthwhile.candidates[1].rollout_completion_time = SimTime{108};
    worthwhile.candidates[1].rollout_distance_m = 540.0;
    evaluate_operational_costs(worthwhile, objective);
    EXPECT_TRUE(worthwhile.candidates[1].material_improvement_passed);
    EXPECT_TRUE(worthwhile.candidates[1].bounded_regret_passed);
    decision = policy.decide(worthwhile, objective, {});
    ASSERT_EQ(decision.action, ActionType::PlanRoute);
    EXPECT_EQ(decision.route, worthwhile.candidates[1].route);
}

TEST(OperationalAgentPolicyTests, CostAwareWaitAgeAndPriorityCanProtectAnOlderAircraft) {
    OperationalCostConfig config;
    config.distance_weight = 0.16;
    config.route_change_weight = 0.06;
    CostAwareCounterfactualPolicy policy{config};
    auto objective = destination_objective();
    auto observation = base_observation();
    observation.current_route = route({1, 2, 4});
    CandidateEvaluation baseline;
    baseline.kind = CandidateKind::ContinueCurrentRoute;
    baseline.route = observation.current_route;
    baseline.counterfactual_evaluated = true;
    baseline.rollout_wait = SimTime{180};
    baseline.rollout_taxi_time = SimTime{100};
    baseline.rollout_completion_time = SimTime{280};
    baseline.rollout_distance_m = 500.0;
    CandidateEvaluation alternative;
    alternative.kind = CandidateKind::AlternateRoute;
    alternative.route = route({1, 3, 4});
    alternative.counterfactual_evaluated = true;
    alternative.rollout_wait = SimTime{90};
    alternative.rollout_taxi_time = SimTime{120};
    alternative.rollout_completion_time = SimTime{300};
    alternative.rollout_distance_m = 550.0;
    observation.candidates = {baseline, alternative};

    evaluate_operational_costs(observation, objective, config);
    EXPECT_FALSE(observation.candidates[1].bounded_regret_passed);
    EXPECT_EQ(policy.decide(observation, objective, {}).route, observation.current_route);

    observation.accumulated_wait_age = SimTime{120};
    objective.priority = 10;
    evaluate_operational_costs(observation, objective, config);
    EXPECT_GT(observation.candidates[1].fairness_wait_multiplier, 1.0);
    EXPECT_TRUE(observation.candidates[1].bounded_regret_passed);
    const auto decision = policy.decide(observation, objective, {});
    EXPECT_EQ(decision.action, ActionType::PlanRoute);
    EXPECT_EQ(decision.route, observation.candidates[1].route);
}

TEST(OperationalAgentPolicyTests, CostAwareCandidateCostCannotMakeAnInfeasibleRouteWin) {
    CostAwareCounterfactualPolicy policy;
    auto observation = base_observation();
    CandidateEvaluation unsafe;
    unsafe.kind = CandidateKind::AlternateRoute;
    unsafe.route = route({1, 3, 4});
    unsafe.valid = false;
    unsafe.counterfactual_evaluated = true;
    unsafe.rollout_wait = SimTime::zero();
    unsafe.rollout_taxi_time = SimTime{1};
    unsafe.rollout_completion_time = SimTime{1};
    unsafe.rollout_distance_m = 1.0;
    CandidateEvaluation safe;
    safe.kind = CandidateKind::ContinueCurrentRoute;
    safe.route = observation.current_route;
    safe.counterfactual_evaluated = true;
    safe.rollout_completed_objectives = 1U;
    safe.rollout_taxi_time = SimTime{100};
    safe.rollout_completion_time = SimTime{100};
    safe.rollout_distance_m = 500.0;
    observation.candidates = {unsafe, safe};
    evaluate_operational_costs(observation, destination_objective());
    EXPECT_FALSE(observation.candidates[0].operational_cost_baseline);
    EXPECT_FALSE(observation.candidates[0].material_improvement_passed);
    const auto decision = policy.decide(observation, destination_objective(), {});
    EXPECT_EQ(decision.action, ActionType::FollowRoute);
    EXPECT_EQ(decision.route, observation.current_route);
}

TEST(OperationalAgentPolicyTests, CostAwareCostCannotTradeAwayBoundedObjectiveCompletion) {
    CostAwareCounterfactualPolicy policy;
    auto observation = base_observation();
    CandidateEvaluation incomplete;
    incomplete.kind = CandidateKind::ContinueCurrentRoute;
    incomplete.route = observation.current_route;
    incomplete.counterfactual_evaluated = true;
    incomplete.rollout_blocked_objectives = 1U;
    incomplete.rollout_taxi_time = SimTime{1};
    incomplete.rollout_completion_time = SimTime{1};
    incomplete.rollout_distance_m = 1.0;
    CandidateEvaluation complete;
    complete.kind = CandidateKind::AlternateRoute;
    complete.route = route({1, 3, 4});
    complete.counterfactual_evaluated = true;
    complete.rollout_completed_objectives = 1U;
    complete.rollout_taxi_time = SimTime{500};
    complete.rollout_completion_time = SimTime{500};
    complete.rollout_distance_m = 1200.0;
    observation.candidates = {incomplete, complete};
    evaluate_operational_costs(observation, destination_objective());
    EXPECT_FALSE(observation.candidates[0].objective_outcome_eligible);
    EXPECT_TRUE(observation.candidates[1].objective_outcome_eligible);
    const auto decision = policy.decide(observation, destination_objective(), {});
    EXPECT_EQ(decision.action, ActionType::PlanRoute);
    EXPECT_EQ(decision.route, complete.route);
}

TEST(OperationalAgentPolicyTests, CostAwareCompleteCostTiesKeepCandidateEnumerationOrder) {
    CostAwareCounterfactualPolicy policy;
    auto observation = base_observation();
    observation.current_route.reset();
    CandidateEvaluation first;
    first.kind = CandidateKind::AlternateRoute;
    first.route = route({1, 2, 4});
    first.counterfactual_evaluated = true;
    first.rollout_completed_objectives = 1U;
    first.rollout_taxi_time = SimTime{100};
    first.rollout_completion_time = SimTime{100};
    first.rollout_distance_m = 500.0;
    first.score_seconds = 1.0;
    CandidateEvaluation second = first;
    second.route = route({1, 3, 4});
    observation.candidates = {first, second};
    evaluate_operational_costs(observation, destination_objective());
    const auto decision = policy.decide(observation, destination_objective(), {});
    EXPECT_EQ(decision.action, ActionType::PlanRoute);
    EXPECT_EQ(decision.route, first.route);
}

TEST(OperationalAgentPolicyTests, SyntheticDownstreamOutcomeCanOverrideLocallyCheapestRoute) {
    CounterfactualPolicy policy;
    auto observation = base_observation();
    CandidateEvaluation locally_cheapest;
    locally_cheapest.kind = CandidateKind::AlternateRoute;
    locally_cheapest.route = route({1, 3, 4});
    locally_cheapest.score_seconds = 20.0; // attractive one-step score
    locally_cheapest.counterfactual_evaluated = true;
    locally_cheapest.rollout_blocked_objectives = 1U;
    locally_cheapest.rollout_wait = SimTime{90};
    locally_cheapest.rollout_taxi_time = SimTime{80};
    locally_cheapest.rollout_distance_m = 180.0;
    CandidateEvaluation downstream_better;
    downstream_better.kind = CandidateKind::AlternateRoute;
    downstream_better.route = route({1, 2, 4});
    downstream_better.score_seconds = 200.0; // locally unattractive
    downstream_better.counterfactual_evaluated = true;
    downstream_better.rollout_completed_objectives = 1U;
    downstream_better.rollout_taxi_time = SimTime{120};
    downstream_better.rollout_distance_m = 240.0;
    observation.current_route.reset();
    observation.candidates = {locally_cheapest, downstream_better};

    const auto decision = policy.decide(observation, destination_objective(), {});
    ASSERT_EQ(decision.action, ActionType::PlanRoute);
    ASSERT_TRUE(decision.route);
    EXPECT_EQ(decision.route->nodes, downstream_better.route->nodes);
}

TEST(OperationalAgentPolicyTests, CounterfactualRejectsUnsafeAndUsesStableOrderForExactTies) {
    CounterfactualPolicy policy;
    auto observation = base_observation();
    observation.candidates.resize(2U);
    observation.candidates[0].kind = CandidateKind::AlternateRoute;
    observation.candidates[0].route = route({1, 3, 4});
    observation.candidates[0].counterfactual_evaluated = true;
    observation.candidates[0].valid = false;
    observation.candidates[1].kind = CandidateKind::ContinueCurrentRoute;
    observation.candidates[1].route = observation.current_route;
    observation.candidates[1].counterfactual_evaluated = true;
    observation.candidates[1].rollout_completed_objectives = 1U;
    const auto decision = policy.decide(observation, destination_objective(), {});
    EXPECT_EQ(decision.action, ActionType::FollowRoute);
    EXPECT_EQ(decision.route, observation.current_route);
}

TEST(OperationalAgentPolicyTests, RollingHorizonPolicyPrefersAPathThatAvoidsForecastEdgeOverlap) {
    RollingHorizonCoordinatedPolicy policy;
    auto observation = base_observation();
    observation.current_route.reset();
    observation.time = SimTime{100};
    observation.candidates = {
        {CandidateKind::AlternateRoute, route({1, 2, 4}), 100.0, SimTime{80}, SimTime{0}, 82.0, true, "short"},
        {CandidateKind::AlternateRoute, route({1, 3, 4}), 120.0, SimTime{100}, SimTime{0}, 102.0, true, "clear"}
    };
    observation.candidates[0].estimated_system_cost_seconds = 220.0;
    observation.candidates[0].estimated_self_delay_seconds = 50.0;
    observation.candidates[0].estimated_wait_imposed_seconds = 88.0;
    observation.candidates[0].predicted_contention_points = {EdgeId{1}};
    observation.candidates[1].estimated_system_cost_seconds = 102.0;
    const auto decision = policy.decide(observation, destination_objective(), {});
    EXPECT_EQ(decision.action, ActionType::PlanRoute);
    EXPECT_EQ(decision.route, observation.candidates[1].route);
    EXPECT_EQ(decision.reason, ReasonCode::ObjectiveCreated);
}

TEST(OperationalAgentPolicyTests, RollingHorizonImmediatelyReplansAfterClosureAndKeepsSafeFallback) {
    RollingHorizonCoordinatedPolicy policy;
    auto observation = base_observation();
    observation.route_invalidated = true;
    observation.invalid_edge = EdgeId{2};
    const auto decision = policy.decide(observation, destination_objective(), {});
    EXPECT_EQ(decision.action, ActionType::PlanRoute);
    EXPECT_EQ(decision.reason, ReasonCode::RouteClosed);
    ASSERT_TRUE(decision.route);
    observation.available_route.reset();
    EXPECT_EQ(policy.decide(observation, destination_objective(), {}).action, ActionType::FailObjective);
}

TEST(OperationalAgentPolicyTests, CoordinatedPolicyCanTradeLocalCostForLowerSystemCost) {
    CoordinatedAdaptiveRoutingPolicy policy;
    auto observation = base_observation();
    observation.current_route.reset();
    observation.candidates = {
        {CandidateKind::AlternateRoute, route({1, 2, 4}), 100.0, SimTime{80}, SimTime{0}, 82.0, true, "shared"},
        {CandidateKind::AlternateRoute, route({1, 3, 4}), 120.0, SimTime{100}, SimTime{0}, 102.0, true, "clear"}
    };
    observation.candidates[0].estimated_system_cost_seconds = 182.0;
    observation.candidates[0].affected_agents = 1;
    observation.candidates[0].conflicting_agent = "aircraft-9";
    observation.candidates[0].conflicting_edge = EdgeId{7};
    observation.candidates[1].estimated_system_cost_seconds = 102.0;

    const auto decision = policy.decide(observation, destination_objective(), {});
    ASSERT_EQ(decision.action, ActionType::PlanRoute);
    EXPECT_EQ(decision.route, observation.candidates[1].route);
    EXPECT_EQ(policy.decide(observation, destination_objective(), {}), decision);
}

TEST(OperationalAgentPolicyTests, CoordinatedPolicyUsesStableCandidateOrderOnSystemCostTie) {
    CoordinatedAdaptiveRoutingPolicy policy;
    auto observation = base_observation();
    observation.current_route.reset();
    observation.candidates = {
        {CandidateKind::AlternateRoute, route({1, 2, 4}), 100.0, SimTime{80}, SimTime{0}, 82.0, true, "first"},
        {CandidateKind::AlternateRoute, route({1, 3, 4}), 100.0, SimTime{80}, SimTime{0}, 82.0, true, "second"}
    };
    observation.candidates[0].estimated_system_cost_seconds = 82.0;
    observation.candidates[1].estimated_system_cost_seconds = 82.0;
    const auto decision = policy.decide(observation, destination_objective(), {});
    EXPECT_EQ(decision.route, observation.candidates.front().route);
}

TEST(OperationalAgentPolicyTests, CoordinatedPolicyDoesNotTreatWaitAsZeroSystemCost) {
    CoordinatedAdaptiveRoutingPolicy policy;
    auto observation = base_observation();
    observation.reservation_blocked = true;
    observation.candidates = {
        {CandidateKind::AlternateRoute, route({1, 3, 4}), 100.0, SimTime{100}, SimTime{0}, 102.0, true, "detour"},
        {CandidateKind::Wait, route({1, 2, 4}), 100.0, SimTime{80}, SimTime{60}, 142.0, true, "wait"}
    };
    observation.candidates[0].estimated_system_cost_seconds = 102.0;
    observation.candidates[1].estimated_system_cost_seconds = 142.0;
    const auto decision = policy.decide(observation, destination_objective(), {});
    EXPECT_EQ(decision.action, ActionType::PlanRoute);
    EXPECT_EQ(decision.route, observation.candidates[0].route);
}

TEST(OperationalAgentPolicyTests, CoordinatedPolicyCanTradeLocalCostForLowerSharedSystemCost) {
    CoordinatedAdaptiveRoutingPolicy policy;
    auto observation = base_observation();
    observation.current_route.reset();
    observation.candidates = {
        {CandidateKind::AlternateRoute, route({1, 2, 4}), 100.0, SimTime{100}, SimTime{0}, 100.0, true, "short shared route"},
        {CandidateKind::AlternateRoute, route({1, 3, 4}), 160.0, SimTime{160}, SimTime{0}, 160.0, true, "clear route"}
    };
    observation.candidates[0].estimated_system_cost_seconds = 250.0;
    observation.candidates[0].estimated_wait_imposed_seconds = 150.0;
    observation.candidates[0].affected_agents = 1;
    observation.candidates[0].conflicting_agent = "aircraft-2";
    observation.candidates[0].conflicting_edge = EdgeId{3};
    observation.candidates[1].estimated_system_cost_seconds = 160.0;

    const auto decision = policy.decide(observation, destination_objective(), {});
    EXPECT_EQ(decision.action, ActionType::PlanRoute);
    EXPECT_EQ(decision.route, observation.candidates[1].route);
    EXPECT_EQ(decision.reason, ReasonCode::Congestion);
}

TEST(OperationalAgentPolicyTests, CoordinatedCostDecisionAndExplanationAreStable) {
    CoordinatedAdaptiveRoutingPolicy policy;
    auto observation = base_observation();
    observation.current_route.reset();
    observation.candidates = {
        {CandidateKind::AlternateRoute, route({1, 2, 4}), 100.0, SimTime{100}, SimTime{0}, 100.0, true, "shared"},
        {CandidateKind::AlternateRoute, route({1, 3, 4}), 160.0, SimTime{160}, SimTime{0}, 160.0, true, "clear"}
    };
    observation.candidates[0].estimated_system_cost_seconds = 251.0;
    observation.candidates[0].estimated_wait_imposed_seconds = 151.0;
    observation.candidates[0].affected_agents = 1;
    observation.candidates[1].estimated_system_cost_seconds = 160.0;
    const auto first = policy.decide(observation, destination_objective(), {});
    const auto second = policy.decide(observation, destination_objective(), {});
    EXPECT_EQ(first, second);
    EXPECT_NE(first.explanation.find("aggregate-cost arbitration"), std::string::npos);
}

TEST(OperationalAgentPolicyTests, CoordinatedPolicyCanTemporarilyHoldForLowerSharedCost) {
    CoordinatedAdaptiveRoutingPolicy policy;
    auto observation = base_observation();
    observation.reservation_blocked = true;
    observation.candidates = {
        {CandidateKind::AlternateRoute, route({1, 3, 4}), 120.0, SimTime{120}, SimTime{0}, 122.0, true, "detour"},
        {CandidateKind::Wait, route({1, 2, 4}), 100.0, SimTime{100}, SimTime{10}, 112.0, true, "short hold"}
    };
    observation.candidates[0].estimated_system_cost_seconds = 122.0;
    observation.candidates[1].estimated_system_cost_seconds = 112.0;

    const auto decision = policy.decide(observation, destination_objective(), {});
    EXPECT_EQ(decision.action, ActionType::Wait);
    EXPECT_EQ(decision.reason, ReasonCode::ReservationBlocked);
}

TEST(OperationalAgentPolicyTests, CoordinationPriorityUsesWaitThenObjectivePriorityThenStableId) {
    const CoordinationPriority old_waiter{SimTime{30}, 0, 9};
    const CoordinationPriority new_high_priority{SimTime{10}, 100, 1};
    const CoordinationPriority high_priority{SimTime{30}, 1, 9};
    const CoordinationPriority low_id{SimTime{30}, 1, 2};
    EXPECT_TRUE(coordination_precedes(old_waiter, new_high_priority));
    EXPECT_TRUE(coordination_precedes(high_priority, old_waiter));
    EXPECT_TRUE(coordination_precedes(low_id, high_priority));
    EXPECT_FALSE(coordination_precedes(high_priority, low_id));
}

}  // namespace
}  // namespace airside::agents
