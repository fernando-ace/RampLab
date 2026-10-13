#pragma once

#include "airside/routing/astar.hpp"

#include <cstdint>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace airside::agents {

enum class AgentType { AircraftSurface, GroundVehicle, OperationalCoordinator };
enum class AgentStatus { Idle, Planning, Executing, Waiting, Replanning, Blocked, Completed, Failed };
enum class ObjectiveType { ReachDestination, ReachAssignedStand, DepartAirport, CompleteAssignedMission };
enum class ActionType { PlanRoute, FollowRoute, Wait, Yield, CompleteObjective, FailObjective };
enum class ReasonCode {
    ObjectiveCreated,
    RouteValid,
    RouteClosed,
    RouteInvalid,
    Congestion,
    ReservationBlocked,
    DestinationChanged,
    NoSafeRoute,
    ObjectiveCompleted,
    OperatorIntervention
};
enum class CandidateKind { ContinueCurrentRoute, AlternateRoute, Wait };
enum class AutonomyMode { Scripted, Adaptive, CoordinatedAdaptive, RollingHorizonCoordinated, Counterfactual, CostAwareCounterfactual };

// All terms are clipped to [0, 1] before weighting. Limits are the scales at
// which a metric reaches its full normalized value; weights are dimensionless.
struct OperationalCostConfig {
    double wait_weight{0.40};
    double completion_time_weight{0.30};
    double distance_weight{0.18};
    double contention_weight{0.08};
    double route_change_weight{0.04};
    double maximum_wait_seconds{600.0};
    double maximum_completion_seconds{600.0};
    double maximum_distance_m{2000.0};
    double maximum_contention_seconds{600.0};
    double material_improvement{0.01};
    double maximum_regret_ratio{0.85};
    double wait_age_fairness_boost{0.50};
    double priority_fairness_boost{0.25};
    double fairness_wait_age_seconds{120.0};
    std::int64_t maximum_priority{10};
    SimTime replan_cooldown{10};
    auto operator<=>(const OperationalCostConfig&) const = default;
};

struct Objective {
    std::string id;
    ObjectiveType type{ObjectiveType::ReachDestination};
    NodeId destination{};
    std::int64_t priority{};
    auto operator<=>(const Objective&) const = default;
};

struct CandidateEvaluation {
    CandidateKind kind{CandidateKind::AlternateRoute};
    std::optional<Route> route;
    double predicted_taxi_distance_m{};
    SimTime predicted_taxi_time{};
    SimTime expected_wait{};
    double score_seconds{};
    bool valid{true};
    std::string explanation;
    double estimated_system_cost_seconds{};
    double estimated_wait_imposed_seconds{};
    std::size_t affected_agents{};
    std::optional<std::string> conflicting_agent;
    std::optional<EdgeId> conflicting_edge;
    double estimated_self_delay_seconds{};
    std::vector<EdgeId> predicted_contention_points;
    struct ContentionPrediction {
        EdgeId edge{};
        std::string agent_id;
        SimTime self_arrival{};
        SimTime other_arrival{};
        double estimated_self_delay_seconds{};
        double estimated_other_delay_seconds{};
        auto operator<=>(const ContentionPrediction&) const = default;
    };
    std::vector<ContentionPrediction> predicted_contention;
    bool counterfactual_evaluated{};
    std::size_t rollout_completed_objectives{};
    std::size_t rollout_blocked_objectives{};
    SimTime rollout_wait{};
    SimTime rollout_taxi_time{};
    SimTime rollout_completion_time{};
    double rollout_distance_m{};
    std::size_t rollout_events{};
    std::size_t rollout_contention_count{};
    double rollout_contention_delay_seconds{};
    double normalized_wait_cost{};
    double normalized_completion_time_cost{};
    double normalized_distance_cost{};
    double normalized_contention_cost{};
    double route_change_cost{};
    double fairness_wait_multiplier{1.0};
    double priority_fairness_contribution{};
    double total_operational_cost{};
    double improvement_vs_baseline{};
    bool operational_cost_baseline{};
    bool objective_outcome_eligible{};
    bool material_improvement_passed{};
    bool bounded_regret_passed{};
    auto operator<=>(const CandidateEvaluation&) const = default;
};

struct SharedTrafficUse {
    std::string agent_id;
    EdgeId edge{};
    NodeId destination{};
    SimTime expected_release{};
    SimTime wait_age{};
    std::int64_t priority{};
    bool reserved{};
    SimTime estimated_arrival{};
    SimTime estimated_occupancy{};
    auto operator<=>(const SharedTrafficUse&) const = default;
};

struct CoordinationPriority {
    SimTime wait_age{};
    std::int64_t objective_priority{};
    std::uint64_t stable_agent_id{};
    auto operator<=>(const CoordinationPriority&) const = default;
};

[[nodiscard]] bool coordination_precedes(const CoordinationPriority& left,
    const CoordinationPriority& right) noexcept;

// The simulator builds this agent-scoped view from authoritative state. Policies
// never receive mutable simulation objects or reservation maps.
struct Observation {
    std::string agent_id;
    AgentType agent_type{AgentType::AircraftSurface};
    SimTime time{};
    NodeId current_node{};
    NodeId destination{};
    std::optional<Route> current_route;
    std::size_t current_edge_index{};
    bool route_invalidated{};
    std::optional<EdgeId> invalid_edge;
    bool reservation_blocked{};
    SimTime blocked_for{};
    SimTime accumulated_wait_age{};
    SimTime current_route_cost{};
    std::optional<Route> available_route;
    SimTime available_route_cost{};
    double aircraft_speed_mps{5.0};
    std::vector<CandidateEvaluation> candidates;
    std::vector<SharedTrafficUse> shared_traffic;
    bool objective_satisfied{};
    bool operator_triggered{};
    auto operator<=>(const Observation&) const = default;
};

struct PolicyMemory {
    std::optional<SimTime> last_replan_at;
    std::optional<SimTime> last_blockage_decision_at;
    std::optional<NodeId> last_destination;
    std::vector<NodeId> previous_route;
    std::uint64_t decisions{};
};

struct Decision {
    ActionType action{ActionType::Wait};
    AgentStatus status{AgentStatus::Waiting};
    ReasonCode reason{ReasonCode::RouteValid};
    std::optional<Route> route;
    std::string explanation;
    auto operator<=>(const Decision&) const = default;
};

struct DecisionEvent {
    std::string event_id;
    SimTime time{};
    std::string agent_id;
    AgentType agent_type{AgentType::AircraftSurface};
    std::string policy_id;
    Objective objective;
    Observation observation;
    std::vector<CandidateEvaluation> candidates;
    std::optional<std::size_t> selected_candidate;
    ActionType action{ActionType::Wait};
    AgentStatus status{AgentStatus::Waiting};
    ReasonCode reason{ReasonCode::RouteValid};
    NodeId observed_node{};
    NodeId destination{};
    std::optional<EdgeId> invalid_edge;
    std::optional<Route> previous_route;
    std::optional<Route> selected_route;
    bool operator_triggered{};
    std::string result;
    std::string explanation;
    std::string coordination_rule;
    std::optional<std::string> coordination_conflict_agent;
    std::optional<EdgeId> coordination_conflict_edge;
    double estimated_system_cost_seconds{};
    std::size_t affected_agents{};
    std::string safety_constraints;
    SimTime planning_horizon{};
    std::size_t counterfactual_rollouts{};
    std::size_t counterfactual_events{};
    std::int64_t objective_priority{};
    SimTime wait_age{};
    bool cost_aware_evaluated{};
    OperationalCostConfig operational_cost_config{};
    bool differs_from_rolling_horizon{};
    std::string state_identifier;
    std::string replan_trigger;
    std::string tie_break_reason;
    auto operator<=>(const DecisionEvent&) const = default;
};

struct OperationalAgent {
    std::string id;
    AgentType type{AgentType::AircraftSurface};
    Objective objective;
    AgentStatus status{AgentStatus::Idle};
    std::string policy_id;
    PolicyMemory memory;
    std::optional<Route> plan;
    ActionType current_action{ActionType::Wait};
    ReasonCode last_reason{ReasonCode::ObjectiveCreated};
    std::vector<DecisionEvent> history;
};

class DecisionPolicy {
public:
    virtual ~DecisionPolicy() = default;
    [[nodiscard]] virtual std::string_view id() const noexcept = 0;
    [[nodiscard]] virtual Decision decide(const Observation& observation,
        const Objective& objective, const PolicyMemory& memory) const = 0;
};

class ScriptedRoutingPolicy final : public DecisionPolicy {
public:
    [[nodiscard]] std::string_view id() const noexcept override;
    [[nodiscard]] Decision decide(const Observation& observation,
        const Objective& objective, const PolicyMemory& memory) const override;
};

class AdaptiveRoutingPolicy final : public DecisionPolicy {
public:
    explicit AdaptiveRoutingPolicy(SimTime minimum_improvement = SimTime{30},
        SimTime decision_cooldown = SimTime{10});
    [[nodiscard]] std::string_view id() const noexcept override;
    [[nodiscard]] Decision decide(const Observation& observation,
        const Objective& objective, const PolicyMemory& memory) const override;

private:
    SimTime minimum_improvement_;
    SimTime decision_cooldown_;
};

class CoordinatedAdaptiveRoutingPolicy final : public DecisionPolicy {
public:
    [[nodiscard]] std::string_view id() const noexcept override;
    [[nodiscard]] Decision decide(const Observation& observation,
        const Objective& objective, const PolicyMemory& memory) const override;
};

class RollingHorizonCoordinatedPolicy final : public DecisionPolicy {
public:
    [[nodiscard]] std::string_view id() const noexcept override;
    [[nodiscard]] Decision decide(const Observation& observation,
        const Objective& objective, const PolicyMemory& memory) const override;
};

class CounterfactualPolicy final : public DecisionPolicy {
public:
    [[nodiscard]] std::string_view id() const noexcept override;
    [[nodiscard]] Decision decide(const Observation& observation,
        const Objective& objective, const PolicyMemory& memory) const override;
};

class CostAwareCounterfactualPolicy final : public DecisionPolicy {
public:
    explicit CostAwareCounterfactualPolicy(OperationalCostConfig config = {});
    [[nodiscard]] std::string_view id() const noexcept override;
    [[nodiscard]] Decision decide(const Observation& observation,
        const Objective& objective, const PolicyMemory& memory) const override;
private:
    OperationalCostConfig config_;
};

// Scores every evaluated rollout and records the Goal 40 baseline and
// bounded-regret checks directly on the candidate evidence.
void evaluate_operational_costs(Observation& observation, const Objective& objective,
    const OperationalCostConfig& config = {});

[[nodiscard]] const DecisionPolicy& policy_for(AutonomyMode mode);
[[nodiscard]] AutonomyMode parse_autonomy_mode(std::string_view value);
[[nodiscard]] std::string_view to_string(AutonomyMode value) noexcept;
[[nodiscard]] std::string_view to_string(AgentType value) noexcept;
[[nodiscard]] std::string_view to_string(AgentStatus value) noexcept;
[[nodiscard]] std::string_view to_string(ObjectiveType value) noexcept;
[[nodiscard]] std::string_view to_string(ActionType value) noexcept;
[[nodiscard]] std::string_view to_string(ReasonCode value) noexcept;
[[nodiscard]] std::string route_string(const std::optional<Route>& route);
[[nodiscard]] std::string_view to_string(CandidateKind value) noexcept;

}  // namespace airside::agents
