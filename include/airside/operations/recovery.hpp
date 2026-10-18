#pragma once

#include "airside/integration/snapshot.hpp"

#include <cstdint>
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

struct RecoveryCandidate {
    std::string id;
    std::uint8_t tier{};
    std::vector<RecoveryAction> actions;
};

struct RecoveryOutcome {
    std::optional<bool> completed;
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
};

struct RecoveryDecision {
    RecoveryTriage triage{RecoveryTriage::NoImpact};
    std::uint64_t fingerprint{};
    std::vector<RecoveryCandidate> candidates;
    std::optional<std::string> selected_candidate_id;
    std::string reason;
};

[[nodiscard]] RecoveryMode recovery_mode_from_string(const std::string& value);
[[nodiscard]] const char* to_string(RecoveryMode value) noexcept;
[[nodiscard]] RecoveryTriage triage_recovery_state(const RecoveryState& state) noexcept;
[[nodiscard]] std::uint64_t recovery_fingerprint(const RecoveryState& state) noexcept;

}  // namespace airside
