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
enum class AutonomyMode { Scripted, Adaptive };

struct Objective {
    std::string id;
    ObjectiveType type{ObjectiveType::ReachDestination};
    NodeId destination{};
    std::int64_t priority{};
    auto operator<=>(const Objective&) const = default;
};

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
    SimTime current_route_cost{};
    std::optional<Route> available_route;
    SimTime available_route_cost{};
    bool objective_satisfied{};
    bool operator_triggered{};
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

[[nodiscard]] const DecisionPolicy& policy_for(AutonomyMode mode);
[[nodiscard]] AutonomyMode parse_autonomy_mode(std::string_view value);
[[nodiscard]] std::string_view to_string(AutonomyMode value) noexcept;
[[nodiscard]] std::string_view to_string(AgentType value) noexcept;
[[nodiscard]] std::string_view to_string(AgentStatus value) noexcept;
[[nodiscard]] std::string_view to_string(ObjectiveType value) noexcept;
[[nodiscard]] std::string_view to_string(ActionType value) noexcept;
[[nodiscard]] std::string_view to_string(ReasonCode value) noexcept;
[[nodiscard]] std::string route_string(const std::optional<Route>& route);

}  // namespace airside::agents
