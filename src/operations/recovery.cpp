#include "airside/operations/recovery.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <tuple>

namespace airside {
namespace {

void mix(std::uint64_t& hash, std::uint64_t value) noexcept {
    for (unsigned i = 0; i < 8; ++i) {
        hash ^= static_cast<unsigned char>(value & 0xffU);
        hash *= 1099511628211ULL;
        value >>= 8U;
    }
}

void mix(std::uint64_t& hash, const std::string& value) noexcept {
    for (const unsigned char byte : value) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    mix(hash, value.size());
}

bool terminal(AircraftState state) {
    return state == AircraftState::Departed;
}

bool eligible(const ServiceTaskSnapshot& task) {
    return task.status == TaskStatus::Pending || task.status == TaskStatus::Waiting ||
           task.status == TaskStatus::Assigned;
}

bool dependencies_complete(const ServiceTaskSnapshot& task, const TurnaroundSnapshot& turn) {
    return std::ranges::all_of(task.prerequisites, [&turn](TaskId id) {
        const auto found = std::ranges::find(turn.tasks, id, &ServiceTaskSnapshot::id);
        return found != turn.tasks.end() && found->status == TaskStatus::Completed;
    });
}

bool better(const RecoveryOutcome& left, const RecoveryOutcome& right) {
    return std::tuple{left.objectives_failed, left.turnarounds_failed,
                      left.maximum_departure_delay_seconds, left.total_departure_delay_seconds,
                      left.service_wait_seconds, left.gse_travel_m, left.taxi_distance_m,
                      left.action_count} <
           std::tuple{right.objectives_failed, right.turnarounds_failed,
                      right.maximum_departure_delay_seconds, right.total_departure_delay_seconds,
                      right.service_wait_seconds, right.gse_travel_m, right.taxi_distance_m,
                      right.action_count};
}

bool material(const RecoveryOutcome& candidate, const RecoveryOutcome& baseline,
              const RecoverySearchConfig& config) {
    if (candidate.objectives_failed < baseline.objectives_failed ||
        candidate.turnarounds_failed < baseline.turnarounds_failed ||
        candidate.objectives_completed > baseline.objectives_completed ||
        candidate.turnarounds_completed > baseline.turnarounds_completed) return true;
    return baseline.maximum_departure_delay_seconds - candidate.maximum_departure_delay_seconds >=
               config.material_delay_improvement_seconds ||
           baseline.total_departure_delay_seconds - candidate.total_departure_delay_seconds >=
               config.material_delay_improvement_seconds;
}

} // namespace

RecoveryTriage triage_recovery(const RecoveryState& state) {
    if (state.revealed_disruptions.empty()) return RecoveryTriage::NoImpact;
    bool threatened = false;
    bool recoverable = false;
    for (const auto& turn : state.snapshot.turnarounds) {
        const auto aircraft = std::ranges::find(state.snapshot.aircraft, turn.aircraft, &AircraftSnapshot::id);
        if (aircraft == state.snapshot.aircraft.end() || terminal(aircraft->state)) continue;
        for (const auto& task : turn.tasks) {
            if (!eligible(task)) continue;
            threatened = true;
            recoverable = recoverable || task.status == TaskStatus::Pending || task.status == TaskStatus::Waiting;
        }
        if (turn.predicted_late || turn.schedule_slack < SimTime::zero()) threatened = true;
    }
    if (!threatened) return RecoveryTriage::NoImpact;
    if (!recoverable) return RecoveryTriage::Monitor;
    return RecoveryTriage::IntegratedPlanningRequired;
}

std::uint64_t recovery_fingerprint(const RecoveryState& state) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    const auto& snapshot = state.snapshot;
    mix(hash, static_cast<std::uint64_t>(snapshot.simulation_time.count()));
    for (const auto& aircraft : snapshot.aircraft) {
        mix(hash, aircraft.id.value()); mix(hash, static_cast<std::uint64_t>(aircraft.state));
        mix(hash, aircraft.assigned_gate.value());
        mix(hash, static_cast<std::uint64_t>(aircraft.scheduled_arrival.count()));
        mix(hash, static_cast<std::uint64_t>(aircraft.scheduled_departure.count()));
        mix(hash, aircraft.actual_arrival ? static_cast<std::uint64_t>(aircraft.actual_arrival->count()) : UINT64_MAX);
        mix(hash, aircraft.actual_departure ? static_cast<std::uint64_t>(aircraft.actual_departure->count()) : UINT64_MAX);
    }
    for (const auto& turn : snapshot.turnarounds) {
        mix(hash, turn.aircraft.value()); mix(hash, static_cast<std::uint64_t>(turn.state));
        mix(hash, static_cast<std::uint64_t>(turn.estimated_ready_time.count()));
        mix(hash, static_cast<std::uint64_t>(turn.schedule_slack.count()));
        for (const auto& task : turn.tasks) {
            mix(hash, task.id.value()); mix(hash, static_cast<std::uint64_t>(task.type));
            mix(hash, static_cast<std::uint64_t>(task.status));
            mix(hash, task.assigned_vehicle ? task.assigned_vehicle->value() : UINT64_MAX);
            mix(hash, task.reassignments);
            for (const auto dependency : task.prerequisites) mix(hash, dependency.value());
        }
    }
    for (const auto& vehicle : snapshot.vehicles) {
        mix(hash, vehicle.id.value()); mix(hash, static_cast<std::uint64_t>(vehicle.type));
        mix(hash, static_cast<std::uint64_t>(vehicle.state)); mix(hash, vehicle.current_node.value());
        mix(hash, vehicle.assigned_aircraft ? vehicle.assigned_aircraft->value() : UINT64_MAX);
    }
    for (const auto& gate : snapshot.gates) {
        mix(hash, gate.id.value()); mix(hash, gate.available); mix(hash, gate.occupying_aircraft ? gate.occupying_aircraft->value() : UINT64_MAX);
    }
    for (const auto& road : snapshot.roads) { mix(hash, road.id.value()); mix(hash, road.enabled); }
    mix(hash, snapshot.runway_owner ? snapshot.runway_owner->value() : UINT64_MAX);
    for (const auto& id : snapshot.runway_queue) mix(hash, id.value());
    for (const auto& disruption : state.revealed_disruptions) {
        mix(hash, disruption.kind); mix(hash, disruption.target);
        mix(hash, static_cast<std::uint64_t>(disruption.revealed_at.count()));
        mix(hash, static_cast<std::uint64_t>(disruption.magnitude.count()));
    }
    for (const auto& [aircraft, age] : state.disruption_wait_age) {
        mix(hash, aircraft.value()); mix(hash, static_cast<std::uint64_t>(age.count()));
    }
    mix(hash, state.active_plan_id);
    return hash;
}

bool recovery_action_is_safe(const RecoveryState& state, const RecoveryAction& action) {
    const auto turn = std::ranges::find(state.snapshot.turnarounds, action.aircraft, &TurnaroundSnapshot::aircraft);
    const auto aircraft = std::ranges::find(state.snapshot.aircraft, action.aircraft, &AircraftSnapshot::id);
    if (turn == state.snapshot.turnarounds.end() || aircraft == state.snapshot.aircraft.end() || terminal(aircraft->state)) return false;
    switch (action.kind) {
    case RecoveryActionKind::ReassignVehicle: {
        const auto task = std::ranges::find(turn->tasks, action.task, &ServiceTaskSnapshot::id);
        const auto vehicle = std::ranges::find(state.snapshot.vehicles, action.vehicle, &ServiceVehicleSnapshot::id);
        return task != turn->tasks.end() && eligible(*task) && dependencies_complete(*task, *turn) &&
               vehicle != state.snapshot.vehicles.end() && vehicle->type == task->type &&
               vehicle->state == VehicleState::Idle && !vehicle->assigned_aircraft;
    }
    case RecoveryActionKind::ReassignStand: {
        const auto gate = std::ranges::find(state.snapshot.gates, action.gate, &GateSnapshot::id);
        return aircraft->state == AircraftState::Scheduled && gate != state.snapshot.gates.end() &&
               gate->available && !gate->occupying_aircraft;
    }
    case RecoveryActionKind::ResequenceTasks: {
        const auto first = std::ranges::find(turn->tasks, action.task, &ServiceTaskSnapshot::id);
        const auto second = std::ranges::find(turn->tasks, action.second_task, &ServiceTaskSnapshot::id);
        return first != turn->tasks.end() && second != turn->tasks.end() && first != second &&
               eligible(*first) && eligible(*second) && dependencies_complete(*first, *turn) &&
               dependencies_complete(*second, *turn);
    }
    case RecoveryActionKind::ControlledHold:
        return action.hold_duration > SimTime::zero() &&
               (aircraft->state == AircraftState::ReadyForPushback || aircraft->state == AircraftState::AtGate);
    }
    return false;
}

std::vector<RecoveryCandidate> generate_recovery_candidates(const RecoveryState& state,
                                                              const RecoverySearchConfig& config) {
    std::vector<RecoveryCandidate> result{{0, {}, "current_plan"}};
    if (config.maximum_candidates == 0 || config.maximum_plan_depth == 0) return result;
    for (const auto& turn : state.snapshot.turnarounds) {
        const auto aircraft = std::ranges::find(state.snapshot.aircraft, turn.aircraft, &AircraftSnapshot::id);
        if (aircraft == state.snapshot.aircraft.end() || terminal(aircraft->state)) continue;
        for (const auto& task : turn.tasks) {
            for (const auto& vehicle : state.snapshot.vehicles) {
                RecoveryAction action{RecoveryActionKind::ReassignVehicle, turn.aircraft, task.id, vehicle.id};
                if (recovery_action_is_safe(state, action)) result.push_back({1, {action}, "gse_reassignment"});
            }
            if (task.status == TaskStatus::Pending || task.status == TaskStatus::Waiting) {
                for (const auto& other : turn.tasks) {
                    if (task.id >= other.id || !eligible(other) || !dependencies_complete(other, turn)) continue;
                    RecoveryAction action; action.kind = RecoveryActionKind::ResequenceTasks;
                    action.aircraft = turn.aircraft; action.task = task.id; action.second_task = other.id;
                    if (recovery_action_is_safe(state, action)) result.push_back({1, {action}, "task_resequence"});
                }
            }
        }
    }
    for (const auto& aircraft : state.snapshot.aircraft) {
        for (const auto& gate : state.snapshot.gates) {
            RecoveryAction action; action.kind = RecoveryActionKind::ReassignStand;
            action.aircraft = aircraft.id; action.gate = gate.id;
            if (recovery_action_is_safe(state, action)) result.push_back({1, {action}, "stand_reassignment"});
        }
        RecoveryAction hold; hold.kind = RecoveryActionKind::ControlledHold; hold.aircraft = aircraft.id;
        hold.hold_duration = SimTime{30}; hold.reason = "bounded_recovery_hold";
        if (recovery_action_is_safe(state, hold)) result.push_back({1, {hold}, "controlled_hold"});
    }
    std::ranges::sort(result.begin() + 1, result.end(), [](const auto& left, const auto& right) {
        const auto left_kind = left.actions.empty() ? RecoveryActionKind::ReassignVehicle : left.actions.front().kind;
        const auto right_kind = right.actions.empty() ? RecoveryActionKind::ReassignVehicle : right.actions.front().kind;
        return std::tie(left_kind, left.actions, left.label) < std::tie(right_kind, right.actions, right.label);
    });
    if (config.maximum_tier >= 2 && config.maximum_plan_depth >= 2) {
        const auto singles = result;
        for (std::size_t i = 1; i < singles.size(); ++i) for (std::size_t j = i + 1; j < singles.size(); ++j) {
            if (singles[i].actions.front().aircraft != singles[j].actions.front().aircraft) continue;
            RecoveryCandidate combined{2, {singles[i].actions.front(), singles[j].actions.front()},
                singles[i].label + "+" + singles[j].label};
            if (recovery_action_is_safe(state, combined.actions[0]) && recovery_action_is_safe(state, combined.actions[1]))
                result.push_back(std::move(combined));
            if (result.size() >= config.maximum_candidates) break;
        }
    }
    if (result.size() > config.maximum_candidates + 1) result.resize(config.maximum_candidates + 1);
    return result;
}

RecoveryDecision plan_recovery(const RecoveryState& state, const RecoverySearchConfig& config,
    const std::function<RecoveryOutcome(const RecoveryCandidate&)>& evaluate) {
    RecoveryDecision decision;
    decision.fingerprint = recovery_fingerprint(state);
    decision.triage = triage_recovery(state);
    if (decision.triage == RecoveryTriage::NoImpact || decision.triage == RecoveryTriage::Monitor) {
        decision.reason = "triage_skipped_broad_planning";
        return decision;
    }
    const auto candidates = generate_recovery_candidates(state, config);
    decision.candidates_generated = candidates.size();
    if (!evaluate) throw std::invalid_argument("recovery planning requires a private candidate evaluator");
    for (const auto& candidate : candidates) {
        if (candidate.tier > config.maximum_tier || candidate.actions.size() > config.maximum_plan_depth) {
            ++decision.candidates_pruned; continue;
        }
        if (!std::ranges::all_of(candidate.actions, [&state](const auto& action) { return recovery_action_is_safe(state, action); })) {
            ++decision.candidates_pruned; continue;
        }
        decision.tiers_reached = std::max(decision.tiers_reached, candidate.tier);
        auto outcome = evaluate(candidate);
        if (candidate.tier == 0) decision.baseline = outcome;
        decision.evaluations.push_back({candidate, outcome});
    }
    if (decision.evaluations.empty() || decision.evaluations.front().candidate.tier != 0)
        throw std::logic_error("recovery search must retain and evaluate its current-plan baseline first");
    const auto& baseline = decision.evaluations.front().outcome;
    decision.selected = decision.evaluations.front().candidate;
    auto best = baseline;
    for (std::size_t i = 1; i < decision.evaluations.size(); ++i) {
        const auto& evaluation = decision.evaluations[i];
        if (material(evaluation.outcome, baseline, config) && better(evaluation.outcome, best)) {
            best = evaluation.outcome; decision.selected = evaluation.candidate;
        }
    }
    decision.reason = decision.selected.tier == 0 ? "no_materially_better_safe_plan" : "selected_material_safe_improvement";
    return decision;
}

} // namespace airside
