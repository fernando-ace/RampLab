#include "airside/agents/operational_agent.hpp"

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
    if (!observation.current_route)
        return plan(observation, ReasonCode::ObjectiveCreated,
            "Objective has no route yet; selecting the best currently observed route.");
    if (observation.reservation_blocked) {
        const bool cooldown_elapsed = !memory.last_replan_at ||
            observation.time - *memory.last_replan_at >= decision_cooldown_;
        const auto required_improvement = minimum_improvement_;
        const bool materially_better = observation.available_route && cooldown_elapsed &&
            observation.available_route_cost + required_improvement < observation.current_route_cost;
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
    switch (mode) {
    case AutonomyMode::Scripted: return scripted;
    case AutonomyMode::Adaptive: return adaptive;
    }
    throw std::invalid_argument("unknown autonomy mode");
}

AutonomyMode parse_autonomy_mode(std::string_view value) {
    if (value == "SCRIPTED") return AutonomyMode::Scripted;
    if (value == "ADAPTIVE") return AutonomyMode::Adaptive;
    throw std::invalid_argument("routing policy must be SCRIPTED or ADAPTIVE");
}

std::string_view to_string(AutonomyMode value) noexcept {
    switch (value) { case AutonomyMode::Scripted: return "SCRIPTED"; case AutonomyMode::Adaptive: return "ADAPTIVE"; }
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

}  // namespace airside::agents
