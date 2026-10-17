#pragma once

#include "airside/integration/snapshot.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace airside {

enum class RecoveryMode { Reactive, Shadow, Integrated };
enum class RecoveryTriage { NoImpact, Monitor, LocalRecoveryAvailable,
                            IntegratedPlanningRequired, NoFeasibleRecovery };
enum class RecoveryActionKind { ReassignVehicle, ReassignStand, ResequenceTasks, ControlledHold };

struct RevealedDisruption {
    std::string kind;
    std::string target;
    SimTime revealed_at{};
    SimTime magnitude{};
    auto operator<=>(const RevealedDisruption&) const = default;
};

// This is a value snapshot. It owns no references into Simulation and contains
// only facts available at snapshot.simulation_time.
struct RecoveryState {
    SimulationSnapshot snapshot;
    std::vector<RevealedDisruption> revealed_disruptions;
    std::vector<std::pair<AircraftId, SimTime>> disruption_wait_age;
    std::string active_plan_id;
};

struct RecoveryAction {
    RecoveryActionKind kind{};
    AircraftId aircraft;
    TaskId task;
    VehicleId vehicle;
    GateId gate;
    TaskId second_task;
    SimTime hold_duration{};
    std::string reason;
    auto operator<=>(const RecoveryAction&) const = default;
};

struct RecoveryCandidate {
    std::size_t tier{};
    std::vector<RecoveryAction> actions;
    std::string label;
    auto operator<=>(const RecoveryCandidate&) const = default;
};

struct RecoveryOutcome {
    std::size_t objectives_completed{};
    std::size_t objectives_failed{};
    std::size_t turnarounds_completed{};
    std::size_t turnarounds_failed{};
    std::int64_t total_departure_delay_seconds{};
    std::int64_t maximum_departure_delay_seconds{};
    std::int64_t service_wait_seconds{};
    double gse_travel_m{};
    double taxi_distance_m{};
    std::size_t action_count{};
    auto operator<=>(const RecoveryOutcome&) const = default;
};

struct RecoveryEvaluation {
    RecoveryCandidate candidate;
    RecoveryOutcome outcome;
};

struct RecoverySearchConfig {
    std::size_t maximum_candidates{64};
    std::size_t maximum_plan_depth{2};
    std::size_t maximum_tier{2};
    std::int64_t material_delay_improvement_seconds{60};
};

struct RecoveryDecision {
    RecoveryTriage triage{RecoveryTriage::NoImpact};
    std::uint64_t fingerprint{};
    std::vector<RecoveryEvaluation> evaluations;
    RecoveryCandidate selected;
    RecoveryOutcome baseline;
    std::size_t candidates_generated{};
    std::size_t candidates_pruned{};
    std::size_t tiers_reached{};
    std::string reason;
};

[[nodiscard]] RecoveryTriage triage_recovery(const RecoveryState& state);
[[nodiscard]] std::uint64_t recovery_fingerprint(const RecoveryState& state) noexcept;
[[nodiscard]] std::vector<RecoveryCandidate> generate_recovery_candidates(
    const RecoveryState& state, const RecoverySearchConfig& config = {});
[[nodiscard]] bool recovery_action_is_safe(const RecoveryState& state, const RecoveryAction& action);
[[nodiscard]] RecoveryDecision plan_recovery(const RecoveryState& state,
    const RecoverySearchConfig& config,
    const std::function<RecoveryOutcome(const RecoveryCandidate&)>& evaluate);

} // namespace airside
