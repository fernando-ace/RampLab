#include "airside/agents/operational_agent.hpp"

#include <algorithm>
#include <ranges>
#include <stdexcept>

namespace airside::agents {
namespace {

Decision plan(const Observation& observation, ReasonCode reason, std::string explanation) {
    if (!observation.available_route) {
        return {ActionType::FailObjective, AgentStatus::Failed, ReasonCode::NoSafeRoute, std::nullopt,
            "No currently feasible safe route; stop safely and report the objective as failed."};
    }
    return {ActionType::PlanRoute, AgentStatus::Replanning, reason, observation.available_route,
        std::move(explanation)};
}

Decision wait(ReasonCode reason, std::string explanation) {
    return {ActionType::Wait, AgentStatus::Waiting, reason, std::nullopt, std::move(explanation)};
}

}  // namespace

bool coordination_precedes(const CoordinationPriority& left, const CoordinationPriority& right) noexcept {
    if (left.wait_age != right.wait_age) return left.wait_age > right.wait_age;
    if (left.objective_priority != right.objective_priority)
        return left.objective_priority > right.objective_priority;
    return left.stable_agent_id < right.stable_agent_id;
}

std::string_view ScriptedRoutingPolicy::id() const noexcept { return "scripted-routing-v1"; }

Decision ScriptedRoutingPolicy::decide(const Observation& observation,
    const Objective& objective, const PolicyMemory& memory) const {
    if (observation.objective_satisfied)
        return {ActionType::CompleteObjective, AgentStatus::Completed, ReasonCode::ObjectiveCompleted,
            std::nullopt, "Objective reached."};
    if (observation.route_invalidated) {
        const auto reason = observation.operator_triggered ? ReasonCode::OperatorIntervention :
            observation.invalid_edge ? ReasonCode::RouteClosed : ReasonCode::RouteInvalid;
        return plan(observation, reason, observation.operator_triggered
            ? "Operator intervention invalidated the planned route; selecting a safe replacement."
            : observation.invalid_edge ? "Planned edge is closed; selecting a safe replacement."
            : "Planned route no longer reaches the objective; selecting a safe replacement.");
    }
    if (memory.last_destination && *memory.last_destination != objective.destination)
        return plan(observation, ReasonCode::DestinationChanged,
            "Objective destination changed; selecting a route to the new destination.");
    if (observation.reservation_blocked)
        return wait(ReasonCode::ReservationBlocked,
            "The next movement is reserved; hold position until the safety system permits it.");
    if (observation.current_route)
        return {ActionType::FollowRoute, AgentStatus::Executing, ReasonCode::RouteValid,
            observation.current_route, "Continue the existing route while it remains valid."};
    return plan(observation, ReasonCode::ObjectiveCreated,
        "Objective has no route yet; request a route through the simulator planner.");
}

AdaptiveRoutingPolicy::AdaptiveRoutingPolicy(SimTime minimum_improvement, SimTime decision_cooldown)
    : minimum_improvement_(minimum_improvement), decision_cooldown_(decision_cooldown) {
    if (minimum_improvement_ < SimTime::zero() || decision_cooldown_ < SimTime::zero())
        throw std::invalid_argument("adaptive routing thresholds cannot be negative");
}

std::string_view AdaptiveRoutingPolicy::id() const noexcept { return "adaptive-routing-v1"; }

Decision AdaptiveRoutingPolicy::decide(const Observation& observation,
    const Objective& objective, const PolicyMemory& memory) const {
    if (observation.objective_satisfied)
        return {ActionType::CompleteObjective, AgentStatus::Completed, ReasonCode::ObjectiveCompleted,
            std::nullopt, "Objective reached."};
    if (observation.route_invalidated || (memory.last_destination && *memory.last_destination != objective.destination)) {
        const auto reason = observation.operator_triggered ? ReasonCode::OperatorIntervention :
            observation.invalid_edge ? ReasonCode::RouteClosed :
            memory.last_destination && *memory.last_destination != objective.destination
                ? ReasonCode::DestinationChanged : ReasonCode::RouteInvalid;
        return plan(observation, reason, observation.operator_triggered
            ? "Operator intervention invalidated the plan; selecting a safe replacement."
            : observation.invalid_edge ? "Closure invalidated the current route; selecting a safe replacement."
            : "The objective changed; selecting a safe route to its new destination.");
    }
    if (!observation.candidates.empty()) {
        const auto best = std::ranges::min_element(observation.candidates, [](const auto& left, const auto& right) {
            if (left.valid != right.valid) return left.valid;
            return left.score_seconds < right.score_seconds;
        });
        const auto current = std::ranges::find(observation.candidates, CandidateKind::ContinueCurrentRoute,
            &CandidateEvaluation::kind);
        if (best != observation.candidates.end() && best->valid) {
            if (best->kind == CandidateKind::Wait)
                return wait(ReasonCode::ReservationBlocked,
                    "The deterministic wait estimate scores below route alternatives while the reservation remains active.");
            if (best->kind == CandidateKind::ContinueCurrentRoute && best->route)
                return {ActionType::FollowRoute, AgentStatus::Executing, ReasonCode::RouteValid,
                    best->route, "Continue the current route because its weighted operational score is lowest."};
            if (best->kind == CandidateKind::AlternateRoute && observation.current_route) {
                const bool cooldown_elapsed = !memory.last_replan_at ||
                    observation.time - *memory.last_replan_at >= decision_cooldown_;
                const bool materially_better = current == observation.candidates.end() ||
                    best->score_seconds + static_cast<double>(minimum_improvement_.count()) < current->score_seconds;
                if (cooldown_elapsed && materially_better && best->route)
                    return {ActionType::PlanRoute, AgentStatus::Replanning, ReasonCode::Congestion,
                        best->route, "Alternate route has the lowest weighted taxi, distance, and reservation-wait score."};
                if (current != observation.candidates.end() && current->route)
                    return {ActionType::FollowRoute, AgentStatus::Executing, ReasonCode::RouteValid,
                        current->route, "Continue the current route; alternatives do not exceed the improvement threshold."};
            }
            if (best->route) {
                const bool avoided_forecast_wait = std::ranges::any_of(observation.candidates,
                    [&](const CandidateEvaluation& candidate) {
                        return candidate.route && candidate.score_seconds > best->score_seconds &&
                            candidate.expected_wait > best->expected_wait;
                    });
                return {ActionType::PlanRoute, AgentStatus::Replanning,
                    avoided_forecast_wait ? ReasonCode::Congestion : ReasonCode::ObjectiveCreated,
                    best->route, avoided_forecast_wait
                        ? "Selected the lowest-scoring route after accounting for forecast reservation waiting."
                        : "Selected the lowest-scoring currently valid route candidate."};
            }
        }
        if (best != observation.candidates.end() && !best->valid && !observation.available_route)
            return {ActionType::FailObjective, AgentStatus::Failed, ReasonCode::NoSafeRoute, std::nullopt,
                "No clearance-safe route remains around stationary ground vehicles; fail safely at the current node."};
    }
    if (!observation.current_route)
        return plan(observation, ReasonCode::ObjectiveCreated,
            "Objective has no route yet; selecting the best currently observed route.");
    if (observation.reservation_blocked) {
        const bool cooldown_elapsed = !memory.last_replan_at ||
            observation.time - *memory.last_replan_at >= decision_cooldown_;
        const bool materially_better = observation.available_route && cooldown_elapsed &&
            observation.available_route_cost + minimum_improvement_ < observation.current_route_cost;
        if (materially_better)
            return plan(observation, ReasonCode::Congestion,
                "A clear alternative exceeds the configured improvement threshold after sustained blockage.");
        return wait(ReasonCode::ReservationBlocked,
            "Wait for the reserved movement to clear; no stable, meaningfully better route is available.");
    }
    return {ActionType::FollowRoute, AgentStatus::Executing, ReasonCode::RouteValid,
        observation.current_route, "Continue the current route; no meaningful state change requires replanning."};
}

const DecisionPolicy& policy_for(AutonomyMode mode) {
    static const ScriptedRoutingPolicy scripted;
    static const AdaptiveRoutingPolicy adaptive;
    static const CoordinatedAdaptiveRoutingPolicy coordinated;
    static const RollingHorizonCoordinatedPolicy rolling;
    static const CounterfactualPolicy counterfactual;
    switch (mode) {
    case AutonomyMode::Scripted: return scripted;
    case AutonomyMode::Adaptive: return adaptive;
    case AutonomyMode::CoordinatedAdaptive: return coordinated;
    case AutonomyMode::RollingHorizonCoordinated: return rolling;
    case AutonomyMode::Counterfactual: return counterfactual;
    }
    throw std::invalid_argument("unknown autonomy mode");
}

std::string_view CounterfactualPolicy::id() const noexcept { return "counterfactual-operational-planning-v1"; }

Decision CounterfactualPolicy::decide(const Observation& observation,
    const Objective& objective, const PolicyMemory& memory) const {
    if (observation.objective_satisfied)
        return {ActionType::CompleteObjective, AgentStatus::Completed, ReasonCode::ObjectiveCompleted,
            std::nullopt, "Objective reached."};
    if (observation.route_invalidated || (memory.last_destination && *memory.last_destination != objective.destination)) {
        const auto reason = observation.operator_triggered ? ReasonCode::OperatorIntervention :
            observation.invalid_edge ? ReasonCode::RouteClosed :
            memory.last_destination && *memory.last_destination != objective.destination
                ? ReasonCode::DestinationChanged : ReasonCode::RouteInvalid;
        return plan(observation, reason, "Invalidated route is replanned through authoritative feasible route candidates.");
    }
    if (observation.candidates.empty())
        return observation.reservation_blocked
            ? wait(ReasonCode::ReservationBlocked, "No feasible candidate exists; hold for authoritative reservations.")
            : plan(observation, ReasonCode::ObjectiveCreated, "No feasible candidate exists; request the safe route fallback.");

    const auto best = std::ranges::min_element(observation.candidates, [](const auto& left, const auto& right) {
        if (left.valid != right.valid) return left.valid;
        if (left.rollout_completed_objectives != right.rollout_completed_objectives)
            return left.rollout_completed_objectives > right.rollout_completed_objectives;
        if (left.rollout_blocked_objectives != right.rollout_blocked_objectives)
            return left.rollout_blocked_objectives < right.rollout_blocked_objectives;
        if (left.rollout_wait != right.rollout_wait) return left.rollout_wait < right.rollout_wait;
        if (left.rollout_taxi_time != right.rollout_taxi_time) return left.rollout_taxi_time < right.rollout_taxi_time;
        if (left.rollout_distance_m != right.rollout_distance_m) return left.rollout_distance_m < right.rollout_distance_m;
        return false; // stable enumeration order is the final tie break
    });
    if (best == observation.candidates.end() || !best->valid)
        return plan(observation, ReasonCode::NoSafeRoute, "All candidates were rejected by feasibility; safety fallback requested.");
    if (best->kind == CandidateKind::Wait)
        return wait(ReasonCode::ReservationBlocked, "Private rollout selected a temporary hold among safe candidate actions.");
    if (!best->route)
        return wait(ReasonCode::ReservationBlocked, "Selected candidate has no route; hold safely.");
    if (best->kind == CandidateKind::ContinueCurrentRoute)
        return {ActionType::FollowRoute, AgentStatus::Executing, ReasonCode::RouteValid,
            best->route, "Private rollout selected the current feasible route by lexicographic downstream outcome."};
    const auto current = std::ranges::find(observation.candidates, CandidateKind::ContinueCurrentRoute,
        &CandidateEvaluation::kind);
    const bool cooldown_elapsed = !memory.last_replan_at || observation.time - *memory.last_replan_at >= SimTime{10};
    const bool materially_better = current == observation.candidates.end() ||
        best->rollout_completed_objectives > current->rollout_completed_objectives ||
        best->rollout_blocked_objectives < current->rollout_blocked_objectives ||
        (best->rollout_completed_objectives == current->rollout_completed_objectives &&
         best->rollout_blocked_objectives == current->rollout_blocked_objectives &&
         (best->rollout_wait + SimTime{15} < current->rollout_wait ||
          (best->rollout_wait == current->rollout_wait &&
           (best->rollout_taxi_time + SimTime{15} < current->rollout_taxi_time ||
            (best->rollout_taxi_time == current->rollout_taxi_time &&
             best->rollout_distance_m < current->rollout_distance_m)))));
    if (observation.current_route && (!cooldown_elapsed || !materially_better) &&
        current != observation.candidates.end() && current->route)
        return {ActionType::FollowRoute, AgentStatus::Executing, ReasonCode::RouteValid,
            current->route, "Preserve the current route because the safe alternate has no material rollout advantage or cooldown remains."};
    return {ActionType::PlanRoute, AgentStatus::Replanning,
        best->rollout_wait > SimTime::zero() ? ReasonCode::Congestion : ReasonCode::ObjectiveCreated,
        best->route, "Selected a safe route by lexicographic comparison of bounded private simulation rollouts."};
}

std::string_view RollingHorizonCoordinatedPolicy::id() const noexcept {
    return "rolling-horizon-coordinated-v1";
}

Decision RollingHorizonCoordinatedPolicy::decide(const Observation& observation,
    const Objective& objective, const PolicyMemory& memory) const {
    if (observation.objective_satisfied)
        return {ActionType::CompleteObjective, AgentStatus::Completed, ReasonCode::ObjectiveCompleted,
            std::nullopt, "Objective reached."};
    if (observation.route_invalidated || (memory.last_destination && *memory.last_destination != objective.destination)) {
        const auto reason = observation.operator_triggered ? ReasonCode::OperatorIntervention :
            observation.invalid_edge ? ReasonCode::RouteClosed :
            memory.last_destination && *memory.last_destination != objective.destination
                ? ReasonCode::DestinationChanged : ReasonCode::RouteInvalid;
        return plan(observation, reason, "Invalidated route is replanned through the safe route candidates.");
    }
    if (observation.candidates.empty()) {
        if (observation.reservation_blocked)
            return wait(ReasonCode::ReservationBlocked, "No safe horizon candidate is available; wait for authoritative reservations to clear.");
        return plan(observation, ReasonCode::ObjectiveCreated, "No route candidate is available; request the safe fallback route.");
    }
    const auto best = std::ranges::min_element(observation.candidates, [](const auto& left, const auto& right) {
        if (left.valid != right.valid) return left.valid;
        if (left.estimated_system_cost_seconds != right.estimated_system_cost_seconds)
            return left.estimated_system_cost_seconds < right.estimated_system_cost_seconds;
        return left.score_seconds < right.score_seconds;
    });
    if (best == observation.candidates.end() || !best->valid)
        return plan(observation, ReasonCode::ObjectiveCreated, "No feasible safe horizon candidate; request the existing safe fallback.");
    if (best->kind == CandidateKind::Wait)
        return wait(ReasonCode::ReservationBlocked, "Wait is the lowest bounded-horizon cost while the reservation remains active.");
    if (!best->route) return wait(ReasonCode::ReservationBlocked, "Selected horizon candidate has no route; hold safely.");
    const auto current = std::ranges::find(observation.candidates, CandidateKind::ContinueCurrentRoute,
        &CandidateEvaluation::kind);
    if (best->kind == CandidateKind::ContinueCurrentRoute)
        return {ActionType::FollowRoute, AgentStatus::Executing, ReasonCode::RouteValid,
            best->route, "Continue the route with the lowest bounded-horizon fleet cost."};
    const bool cooldown_elapsed = !memory.last_replan_at || observation.time - *memory.last_replan_at >= SimTime{10};
    const bool materially_better = current == observation.candidates.end() ||
        best->estimated_system_cost_seconds + 15.0 < current->estimated_system_cost_seconds;
    if (observation.current_route && (!cooldown_elapsed || !materially_better) &&
        current != observation.candidates.end() && current->route)
        return {ActionType::FollowRoute, AgentStatus::Executing, ReasonCode::RouteValid,
            current->route, "Keep the current route; the forecast gain is below 15 seconds or cooldown."};
    return {ActionType::PlanRoute, AgentStatus::Replanning,
        best->predicted_contention_points.empty() ? ReasonCode::ObjectiveCreated : ReasonCode::Congestion,
        best->route, "Selected the lowest bounded-horizon cost using predicted edge occupancy and stable candidate order."};
}

std::string_view CoordinatedAdaptiveRoutingPolicy::id() const noexcept {
    return "coordinated-adaptive-routing-v1";
}

Decision CoordinatedAdaptiveRoutingPolicy::decide(const Observation& observation,
    const Objective& objective, const PolicyMemory& memory) const {
    if (observation.objective_satisfied)
        return {ActionType::CompleteObjective, AgentStatus::Completed, ReasonCode::ObjectiveCompleted,
            std::nullopt, "Objective reached."};
    if (observation.route_invalidated || (memory.last_destination && *memory.last_destination != objective.destination)) {
        const auto reason = observation.operator_triggered ? ReasonCode::OperatorIntervention :
            observation.invalid_edge ? ReasonCode::RouteClosed :
            memory.last_destination && *memory.last_destination != objective.destination
                ? ReasonCode::DestinationChanged : ReasonCode::RouteInvalid;
        return plan(observation, reason, "Invalidated route is replanned subject to authoritative safety constraints.");
    }
    if (observation.candidates.empty()) {
        static const AdaptiveRoutingPolicy fallback;
        return fallback.decide(observation, objective, memory);
    }
    const auto best = std::ranges::min_element(observation.candidates,
        [](const auto& left, const auto& right) {
            if (left.valid != right.valid) return left.valid;
            if (left.estimated_system_cost_seconds != right.estimated_system_cost_seconds)
                return left.estimated_system_cost_seconds < right.estimated_system_cost_seconds;
            return left.score_seconds < right.score_seconds;
        });
    if (best == observation.candidates.end() || !best->valid)
        return plan(observation, ReasonCode::ObjectiveCreated, "No feasible coordinated candidate; requesting a safe route.");
    if (best->kind == CandidateKind::Wait)
        return wait(ReasonCode::ReservationBlocked,
            "Wait selected by shared-state system-cost arbitration; reservations and safety checks remain authoritative.");
    if (!best->route) return wait(ReasonCode::ReservationBlocked, "Selected candidate has no route; hold safely.");
    const auto current = std::ranges::find(observation.candidates, CandidateKind::ContinueCurrentRoute,
        &CandidateEvaluation::kind);
    if (best->kind == CandidateKind::ContinueCurrentRoute)
        return {ActionType::FollowRoute, AgentStatus::Executing, ReasonCode::RouteValid,
            best->route, "Continue the route with the lowest estimated aggregate taxi and waiting cost."};
    const bool cooldown_elapsed = !memory.last_replan_at ||
        observation.time - *memory.last_replan_at >= SimTime{10};
    const bool materially_better = current == observation.candidates.end() ||
        best->estimated_system_cost_seconds + 30.0 < current->estimated_system_cost_seconds;
    if (observation.current_route && (!cooldown_elapsed || !materially_better) &&
        current != observation.candidates.end() && current->route)
        return {ActionType::FollowRoute, AgentStatus::Executing, ReasonCode::RouteValid,
            current->route, "Keep the current route because coordinated improvement is below threshold or cooldown."};
    const bool contention_detected = std::ranges::any_of(observation.candidates, [](const auto& candidate) {
        return candidate.affected_agents > 0 || candidate.estimated_wait_imposed_seconds > 0.0;
    });
    const auto reason = contention_detected || best->estimated_system_cost_seconds > best->score_seconds
        ? ReasonCode::Congestion : ReasonCode::ObjectiveCreated;
    std::string explanation = "Selected by deterministic aggregate-cost arbitration; this route imposes an estimated " +
        std::to_string(best->estimated_wait_imposed_seconds) + " seconds across " +
        std::to_string(best->affected_agents) + " affected aircraft, with stable candidate-order ties.";
    const auto local_best = std::ranges::min_element(observation.candidates,
        [](const auto& left, const auto& right) {
            if (left.valid != right.valid) return left.valid;
            return left.score_seconds < right.score_seconds;
        });
    if (local_best != observation.candidates.end() && local_best != best && local_best->valid &&
        local_best->estimated_system_cost_seconds > best->estimated_system_cost_seconds) {
        explanation += " The lower local-cost alternative would impose an estimated " +
            std::to_string(local_best->estimated_wait_imposed_seconds) + " seconds across " +
            std::to_string(local_best->affected_agents) + " aircraft; its system cost was " +
            std::to_string(local_best->estimated_system_cost_seconds) + " seconds versus " +
            std::to_string(best->estimated_system_cost_seconds) + " seconds for the selected route.";
    }
    return {ActionType::PlanRoute, AgentStatus::Replanning, reason, best->route, std::move(explanation)};
}

AutonomyMode parse_autonomy_mode(std::string_view value) {
    if (value == "SCRIPTED") return AutonomyMode::Scripted;
    if (value == "ADAPTIVE") return AutonomyMode::Adaptive;
    if (value == "COORDINATED_ADAPTIVE") return AutonomyMode::CoordinatedAdaptive;
    if (value == "ROLLING_HORIZON_COORDINATED") return AutonomyMode::RollingHorizonCoordinated;
    if (value == "COUNTERFACTUAL") return AutonomyMode::Counterfactual;
    throw std::invalid_argument("routing policy must be SCRIPTED, ADAPTIVE, COORDINATED_ADAPTIVE, ROLLING_HORIZON_COORDINATED, or COUNTERFACTUAL");
}

std::string_view to_string(AutonomyMode value) noexcept {
    switch (value) { case AutonomyMode::Scripted: return "SCRIPTED"; case AutonomyMode::Adaptive: return "ADAPTIVE"; case AutonomyMode::CoordinatedAdaptive: return "COORDINATED_ADAPTIVE"; case AutonomyMode::RollingHorizonCoordinated: return "ROLLING_HORIZON_COORDINATED"; case AutonomyMode::Counterfactual: return "COUNTERFACTUAL"; }
    return "UNKNOWN";
}
std::string_view to_string(AgentType value) noexcept {
    switch (value) { case AgentType::AircraftSurface: return "AIRCRAFT_SURFACE"; case AgentType::GroundVehicle: return "GROUND_VEHICLE"; case AgentType::OperationalCoordinator: return "OPERATIONAL_COORDINATOR"; }
    return "UNKNOWN";
}
std::string_view to_string(AgentStatus value) noexcept {
    switch (value) { case AgentStatus::Idle: return "IDLE"; case AgentStatus::Planning: return "PLANNING"; case AgentStatus::Executing: return "EXECUTING"; case AgentStatus::Waiting: return "WAITING"; case AgentStatus::Replanning: return "REPLANNING"; case AgentStatus::Blocked: return "BLOCKED"; case AgentStatus::Completed: return "COMPLETED"; case AgentStatus::Failed: return "FAILED"; }
    return "UNKNOWN";
}
std::string_view to_string(ObjectiveType value) noexcept {
    switch (value) { case ObjectiveType::ReachDestination: return "REACH_DESTINATION"; case ObjectiveType::ReachAssignedStand: return "REACH_ASSIGNED_STAND"; case ObjectiveType::DepartAirport: return "DEPART_AIRPORT"; case ObjectiveType::CompleteAssignedMission: return "COMPLETE_ASSIGNED_MISSION"; }
    return "UNKNOWN";
}
std::string_view to_string(ActionType value) noexcept {
    switch (value) { case ActionType::PlanRoute: return "PLAN_ROUTE"; case ActionType::FollowRoute: return "FOLLOW_ROUTE"; case ActionType::Wait: return "WAIT"; case ActionType::Yield: return "YIELD"; case ActionType::CompleteObjective: return "COMPLETE_OBJECTIVE"; case ActionType::FailObjective: return "FAIL_OBJECTIVE"; }
    return "UNKNOWN";
}
std::string_view to_string(ReasonCode value) noexcept {
    switch (value) {
    case ReasonCode::ObjectiveCreated: return "OBJECTIVE_CREATED";
    case ReasonCode::RouteValid: return "ROUTE_VALID";
    case ReasonCode::RouteClosed: return "ROUTE_CLOSED";
    case ReasonCode::RouteInvalid: return "ROUTE_INVALID";
    case ReasonCode::Congestion: return "CONGESTION";
    case ReasonCode::ReservationBlocked: return "RESERVATION_BLOCKED";
    case ReasonCode::DestinationChanged: return "DESTINATION_CHANGED";
    case ReasonCode::NoSafeRoute: return "NO_SAFE_ROUTE";
    case ReasonCode::ObjectiveCompleted: return "OBJECTIVE_COMPLETED";
    case ReasonCode::OperatorIntervention: return "OPERATOR_INTERVENTION";
    }
    return "UNKNOWN";
}

std::string route_string(const std::optional<Route>& route) {
    if (!route) return "";
    std::string result;
    for (const auto node : route->nodes) {
        if (!result.empty()) result += "->";
        result += std::to_string(node.value());
    }
    return result;
}

std::string_view to_string(CandidateKind value) noexcept {
    switch (value) {
    case CandidateKind::ContinueCurrentRoute: return "CONTINUE_CURRENT_ROUTE";
    case CandidateKind::AlternateRoute: return "ALTERNATE_ROUTE";
    case CandidateKind::Wait: return "WAIT";
    }
    return "UNKNOWN";
}

}  // namespace airside::agents
