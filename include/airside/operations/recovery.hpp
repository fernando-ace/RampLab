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
    SimTime revealed_at{};
    std::string kind;
    std::string entity;
    std::string detail;
    auto operator<=>(const RevealedDisruption&) const = default;
};

// An immutable-by-convention owned decision boundary. Construction copies the
// simulator snapshot; no simulator-owned mutable objects escape through it.
struct RecoveryState {
    SimulationSnapshot snapshot;
    std::vector<RevealedDisruption> revealed_disruptions;
    std::string active_plan_id;
};

struct RecoveryAction {
    RecoveryActionKind kind{RecoveryActionKind::ReassignVehicle};
    std::string task_id;
    std::string target_id;
    std::string reason;
    auto operator<=>(const RecoveryAction&) const = default;
};

struct RecoveryActionResult {
    bool accepted{};
    std::string reason;
};

struct RecoveryCandidate {
    std::string id;
    std::uint8_t tier{};
    std::vector<RecoveryAction> actions;
    auto operator<=>(const RecoveryCandidate&) const = default;
};

struct RecoveryOutcome {
    std::optional<bool> completed;
    std::uint32_t objective_failures{};
    std::uint32_t timeouts{};
    std::uint32_t turnaround_failures{};
    std::uint32_t completed_objectives{};
    std::optional<double> worst_departure_delay_seconds;
    std::optional<double> departure_delay_seconds;
    std::optional<double> service_wait_seconds;
    std::optional<double> gse_travel_seconds;
};

struct RecoveryEvaluation {
    RecoveryCandidate candidate;
    RecoveryOutcome outcome;
    bool valid{};
    std::string reason;
    std::uint64_t private_events{};
    SimTime simulated_horizon{};
    std::string termination_reason;
};

struct RecoverySearchConfig {
    std::size_t max_candidates{8};
    std::size_t max_plan_depth{2};
    std::uint8_t max_search_tier{2};
    std::uint64_t max_private_events{10000};
    SimTime max_future_simulated_time{SimTime{3600}};
    double minimum_delay_improvement_seconds{300.0};
};

struct RecoveryDecision {
    RecoveryTriage triage{RecoveryTriage::NoImpact};
    std::uint64_t fingerprint{};
    std::vector<RecoveryCandidate> candidates;
    std::vector<RecoveryEvaluation> evaluations;
    std::size_t candidates_pruned{};
    std::optional<std::string> selected_candidate_id;
    std::string reason;
};

struct RecoveryEpochRecord {
    SimTime time{};
    std::string trigger;
    RecoveryMode mode{RecoveryMode::Reactive};
    RecoveryTriage triage{RecoveryTriage::NoImpact};
    std::uint64_t fingerprint{};
    bool suppressed{};
    bool planned{};
    std::string skip_reason;
    std::size_t candidate_count{};
    std::optional<std::string> selected_candidate_id;
    std::string application_result;
};

[[nodiscard]] std::vector<RecoveryCandidate> generate_recovery_candidates(
    const RecoveryState& state, const RecoverySearchConfig& config);
[[nodiscard]] RecoveryDecision plan_recovery(
    const RecoveryState& state, const RecoverySearchConfig& config,
    const std::function<RecoveryEvaluation(const RecoveryCandidate&)>& evaluate);

[[nodiscard]] RecoveryMode recovery_mode_from_string(const std::string& value);
[[nodiscard]] const char* to_string(RecoveryMode value) noexcept;
[[nodiscard]] RecoveryTriage triage_recovery_state(const RecoveryState& state) noexcept;
[[nodiscard]] std::uint64_t recovery_fingerprint(const RecoveryState& state) noexcept;

}  // namespace airside
