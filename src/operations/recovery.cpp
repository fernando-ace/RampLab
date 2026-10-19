#include "airside/operations/recovery.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace airside {
namespace {
void hash_byte(std::uint64_t& hash, std::uint8_t byte) noexcept {
    hash ^= byte;
    hash *= 1099511628211ULL;
}
void hash_u64(std::uint64_t& hash, std::uint64_t value) noexcept {
    for (unsigned shift = 0; shift < 64; shift += 8)
        hash_byte(hash, static_cast<std::uint8_t>((value >> shift) & 0xffU));
}
void hash_string(std::uint64_t& hash, std::string_view value) noexcept {
    hash_u64(hash, value.size());
    for (const auto ch : value) hash_byte(hash, static_cast<std::uint8_t>(ch));
}
}

RecoveryMode recovery_mode_from_string(const std::string& value) {
    if (value == "REACTIVE") return RecoveryMode::Reactive;
    if (value == "RECOVERY_SHADOW") return RecoveryMode::Shadow;
    if (value == "INTEGRATED_RECOVERY") return RecoveryMode::Integrated;
    throw std::invalid_argument("unknown recovery mode '" + value + "'");
}

const char* to_string(RecoveryMode value) noexcept {
    switch (value) {
    case RecoveryMode::Reactive: return "REACTIVE";
    case RecoveryMode::Shadow: return "RECOVERY_SHADOW";
    case RecoveryMode::Integrated: return "INTEGRATED_RECOVERY";
    }
    return "REACTIVE";
}

std::uint64_t recovery_fingerprint(const RecoveryState& state) noexcept {
    // FNV-1a over sorted, decision-relevant snapshot content. Diagnostics and
    // display-only metadata are intentionally omitted.
    std::uint64_t hash = 14695981039346656037ULL;
    hash_u64(hash, static_cast<std::uint64_t>(state.snapshot.simulation_time.count()));
    auto aircraft = state.snapshot.aircraft;
    std::ranges::sort(aircraft, {}, &AircraftSnapshot::id);
    for (const auto& item : aircraft) {
        hash_u64(hash, item.id.value()); hash_u64(hash, static_cast<std::uint64_t>(item.state));
        hash_u64(hash, item.assigned_gate.value());
    }
    auto turnarounds = state.snapshot.turnarounds;
    std::ranges::sort(turnarounds, {}, &TurnaroundSnapshot::aircraft);
    for (const auto& turnaround : turnarounds) {
        hash_u64(hash, turnaround.aircraft.value());
        hash_u64(hash, static_cast<std::uint64_t>(turnaround.state));
        auto tasks = turnaround.tasks;
        std::ranges::sort(tasks, {}, &ServiceTaskSnapshot::id);
        for (const auto& task : tasks) {
            hash_u64(hash, task.id.value()); hash_u64(hash, static_cast<std::uint64_t>(task.status));
            hash_u64(hash, task.assigned_vehicle ? task.assigned_vehicle->value() : 0);
        }
    }
    auto vehicles = state.snapshot.vehicles;
    std::ranges::sort(vehicles, {}, &ServiceVehicleSnapshot::id);
    for (const auto& vehicle : vehicles) {
        hash_u64(hash, vehicle.id.value()); hash_u64(hash, static_cast<std::uint64_t>(vehicle.state));
        hash_u64(hash, vehicle.current_node.value());
        hash_u64(hash, vehicle.assigned_aircraft ? vehicle.assigned_aircraft->value() : 0);
    }
    auto gates = state.snapshot.gates;
    std::ranges::sort(gates, {}, &GateSnapshot::id);
    for (const auto& gate : gates) {
        hash_u64(hash, gate.id.value()); hash_u64(hash, gate.available ? 1 : 0);
        hash_u64(hash, gate.occupying_aircraft ? gate.occupying_aircraft->value() : 0);
    }
    auto roads = state.snapshot.roads;
    std::ranges::sort(roads, {}, &RoadEdgeSnapshot::id);
    for (const auto& road : roads) { hash_u64(hash, road.id.value()); hash_u64(hash, road.enabled ? 1 : 0); }
    auto disruptions = state.revealed_disruptions;
    std::erase_if(disruptions, [&](const auto& disruption) {
        return disruption.revealed_at > state.snapshot.simulation_time;
    });
    std::ranges::sort(disruptions);
    for (const auto& disruption : disruptions) {
        hash_u64(hash, static_cast<std::uint64_t>(disruption.revealed_at.count()));
        hash_string(hash, disruption.kind); hash_string(hash, disruption.entity); hash_string(hash, disruption.detail);
    }
    hash_string(hash, state.active_plan_id);
    return hash;
}

RecoveryTriage triage_recovery_state(const RecoveryState& state) noexcept {
    bool threatened = false;
    bool relevant = false;
    bool task_slowdown_revealed = false;
    for (const auto& disruption : state.revealed_disruptions) {
        if (disruption.revealed_at > state.snapshot.simulation_time) continue;
        relevant = true;
        if (disruption.kind == "vehicle_outage" || disruption.kind == "stand_closure" ||
            disruption.kind == "surface_closure" || disruption.kind == "late_inbound" ||
            disruption.kind == "task_slowdown" || disruption.kind == "task_duration_slowdown") threatened = true;
        if (disruption.kind == "task_slowdown" || disruption.kind == "task_duration_slowdown")
            task_slowdown_revealed = true;
    }
    if (!relevant || !threatened) return RecoveryTriage::NoImpact;
    for (const auto& disruption : state.revealed_disruptions) {
        if (disruption.revealed_at > state.snapshot.simulation_time || disruption.kind != "stand_closure")
            continue;
        if (std::ranges::any_of(state.snapshot.aircraft, [&](const auto& aircraft) {
            return std::to_string(aircraft.assigned_gate.value()) == disruption.entity &&
                aircraft.state != AircraftState::Departed && aircraft.surface_state != "ParkedAtGate" &&
                aircraft.surface_state != "Arrived";
        })) return RecoveryTriage::IntegratedPlanningRequired;
    }
    const bool surface_closure_revealed = std::ranges::any_of(state.revealed_disruptions,
        [&](const auto& disruption) {
            return disruption.revealed_at <= state.snapshot.simulation_time && disruption.kind == "surface_closure";
        });
    if (surface_closure_revealed && std::ranges::any_of(state.snapshot.aircraft, [](const auto& aircraft) {
        return aircraft.surface_state == "ReadyForPushback" || aircraft.surface_state == "WaitingForPushback";
    })) return RecoveryTriage::IntegratedPlanningRequired;
    for (const auto& turnaround : state.snapshot.turnarounds) {
        if (turnaround.predicted_late)
            return RecoveryTriage::IntegratedPlanningRequired;
        if (task_slowdown_revealed && std::ranges::count_if(turnaround.tasks, [](const auto& task) {
                return task.status == TaskStatus::Pending || task.status == TaskStatus::Waiting;
            }) >= 2) return RecoveryTriage::IntegratedPlanningRequired;
        for (const auto& task : turnaround.tasks) {
            if (task.status == TaskStatus::InProgress || task.status == TaskStatus::Waiting)
                return RecoveryTriage::LocalRecoveryAvailable;
        }
    }
    return RecoveryTriage::Monitor;
}

namespace {
std::string id_text(std::uint64_t value) { return std::to_string(value); }

bool task_is_pending(TaskStatus status) {
    return status == TaskStatus::Pending || status == TaskStatus::Waiting;
}

bool vehicle_outage_revealed(const RecoveryState& state, VehicleId vehicle) {
    const auto target = "vehicle:" + std::to_string(vehicle.value());
    return std::ranges::any_of(state.revealed_disruptions, [&](const auto& disruption) {
        return disruption.revealed_at <= state.snapshot.simulation_time &&
            disruption.kind == "vehicle_outage" &&
            (disruption.entity == target || disruption.entity == std::to_string(vehicle.value()));
    });
}

bool dependencies_complete(const ServiceTaskSnapshot& task,
    const std::vector<ServiceTaskSnapshot>& tasks) {
    return std::ranges::all_of(task.prerequisites, [&](TaskId prerequisite) {
        const auto it = std::ranges::find(tasks, prerequisite, &ServiceTaskSnapshot::id);
        return it != tasks.end() && it->status == TaskStatus::Completed;
    });
}

bool evaluation_better(const RecoveryOutcome& lhs, const RecoveryOutcome& rhs) {
    if (lhs.objective_failures != rhs.objective_failures)
        return lhs.objective_failures < rhs.objective_failures;
    if (lhs.timeouts != rhs.timeouts) return lhs.timeouts < rhs.timeouts;
    if (lhs.turnaround_failures != rhs.turnaround_failures)
        return lhs.turnaround_failures < rhs.turnaround_failures;
    if (lhs.completed_objectives != rhs.completed_objectives)
        return lhs.completed_objectives > rhs.completed_objectives;
    const auto delay = [](const auto& value) {
        return value.value_or(std::numeric_limits<double>::infinity());
    };
    if (delay(lhs.worst_departure_delay_seconds) != delay(rhs.worst_departure_delay_seconds))
        return delay(lhs.worst_departure_delay_seconds) < delay(rhs.worst_departure_delay_seconds);
    if (delay(lhs.departure_delay_seconds) != delay(rhs.departure_delay_seconds))
        return delay(lhs.departure_delay_seconds) < delay(rhs.departure_delay_seconds);
    if (lhs.service_wait_seconds.value_or(std::numeric_limits<double>::infinity()) !=
        rhs.service_wait_seconds.value_or(std::numeric_limits<double>::infinity()))
        return lhs.service_wait_seconds.value_or(std::numeric_limits<double>::infinity()) <
            rhs.service_wait_seconds.value_or(std::numeric_limits<double>::infinity());
    return lhs.gse_travel_seconds.value_or(std::numeric_limits<double>::infinity()) <
        rhs.gse_travel_seconds.value_or(std::numeric_limits<double>::infinity());
}

bool material_improvement(const RecoveryOutcome& candidate, const RecoveryOutcome& baseline,
    const RecoverySearchConfig& config) {
    if (candidate.objective_failures < baseline.objective_failures ||
        candidate.timeouts < baseline.timeouts ||
        candidate.turnaround_failures < baseline.turnaround_failures ||
        candidate.completed_objectives > baseline.completed_objectives) return true;
    const auto candidate_delay = candidate.worst_departure_delay_seconds ?
        candidate.worst_departure_delay_seconds : candidate.departure_delay_seconds;
    const auto baseline_delay = baseline.worst_departure_delay_seconds ?
        baseline.worst_departure_delay_seconds : baseline.departure_delay_seconds;
    return candidate_delay && baseline_delay &&
        *baseline_delay - *candidate_delay >= config.minimum_delay_improvement_seconds;
}
}  // namespace

std::vector<RecoveryCandidate> generate_recovery_candidates(
    const RecoveryState& state, const RecoverySearchConfig& config) {
    std::vector<RecoveryCandidate> result;
    result.push_back({"current_plan", 0, {}});
    if (config.max_candidates <= 1 || config.max_plan_depth == 0 || config.max_search_tier == 0)
        return result;

    std::vector<RecoveryCandidate> executable;
    auto turnarounds = state.snapshot.turnarounds;
    std::ranges::sort(turnarounds, {}, &TurnaroundSnapshot::aircraft);
    auto vehicles = state.snapshot.vehicles;
    std::ranges::sort(vehicles, {}, &ServiceVehicleSnapshot::id);
    auto gates = state.snapshot.gates;
    std::ranges::sort(gates, {}, &GateSnapshot::id);

    for (const auto& turnaround : turnarounds) {
        auto tasks = turnaround.tasks;
        std::ranges::sort(tasks, {}, &ServiceTaskSnapshot::id);
        for (const auto& task : tasks) {
            const bool pending = task_is_pending(task.status);
            const bool stranded = (task.status == TaskStatus::Assigned || task.status == TaskStatus::InProgress) &&
                task.assigned_vehicle &&
                vehicle_outage_revealed(state, *task.assigned_vehicle);
            if ((!pending && !stranded) || !dependencies_complete(task, tasks)) continue;
            for (const auto& vehicle : vehicles) {
                if ((task.assigned_vehicle && vehicle.id == *task.assigned_vehicle) ||
                    vehicle.type != task.type || vehicle.state != VehicleState::Idle || vehicle.assigned_aircraft ||
                    vehicle.fleet_status == "Unavailable")
                    continue;
                const auto task_id = id_text(task.id.value());
                const auto vehicle_id = id_text(vehicle.id.value());
                executable.push_back({"vehicle:" + task_id + ":" + vehicle_id, 1,
                    {{RecoveryActionKind::ReassignVehicle, task_id, vehicle_id,
                      stranded ? "replace vehicle stranded by revealed outage" : "compatible idle vehicle"}}});
            }
        }
        const auto aircraft = std::ranges::find(state.snapshot.aircraft, turnaround.aircraft,
            &AircraftSnapshot::id);
        const bool affected_by_stand_closure = aircraft != state.snapshot.aircraft.end() &&
            std::ranges::any_of(state.revealed_disruptions, [&](const auto& disruption) {
                return disruption.kind == "stand_closure" &&
                    disruption.entity == id_text(aircraft->assigned_gate.value());
            });
        if (affected_by_stand_closure && aircraft->state != AircraftState::Departed &&
            aircraft->surface_state != "ParkedAtGate" && aircraft->surface_state != "Arrived") {
            for (const auto& target : gates) {
                if (target.id == aircraft->assigned_gate || !target.available || target.occupying_aircraft)
                    continue;
                const auto from = std::ranges::find(gates, aircraft->assigned_gate, &GateSnapshot::id);
                if (from == gates.end()) continue;
                executable.push_back({"stand:" + id_text(aircraft->id.value()) + ":" +
                    id_text(target.id.value()), 1, {{RecoveryActionKind::ReassignStand,
                    id_text(aircraft->id.value()), id_text(target.id.value()), "available target stand"}}});
            }
        }
        std::vector<const ServiceTaskSnapshot*> pending;
        for (const auto& task : tasks)
            if (task_is_pending(task.status) && dependencies_complete(task, tasks)) pending.push_back(&task);
        for (std::size_t target = 0; target < pending.size(); ++target) {
            for (std::size_t promoted = target + 1; promoted < pending.size(); ++promoted) {
                executable.push_back({"resequence:" + id_text(pending[promoted]->id.value()) + ":" +
                    id_text(pending[target]->id.value()), 1, {{RecoveryActionKind::ResequenceTasks,
                    id_text(pending[promoted]->id.value()), id_text(pending[target]->id.value()),
                    "prefer dependency-valid task before target"}}});
            }
        }
        const bool departure_disruption_revealed = std::ranges::any_of(state.revealed_disruptions,
            [&](const auto& disruption) {
                return disruption.revealed_at <= state.snapshot.simulation_time &&
                    disruption.kind == "surface_closure";
            });
        if (departure_disruption_revealed && aircraft != state.snapshot.aircraft.end() &&
            (aircraft->surface_state == "ReadyForPushback" || aircraft->surface_state == "WaitingForPushback") &&
            turnaround.state != TurnaroundState::Departed && turnaround.state != TurnaroundState::Failed) {
            executable.push_back({"hold:" + id_text(turnaround.aircraft.value()), 1,
                {{RecoveryActionKind::ControlledHold, id_text(turnaround.aircraft.value()), "60",
                "60 second hold while a revealed surface closure affects departure operations"}}});
        }
    }

    const auto outage_for_candidate = [&](const RecoveryCandidate& candidate) {
        if (candidate.actions.empty() || candidate.actions.front().kind != RecoveryActionKind::ReassignVehicle)
            return false;
        const auto& action = candidate.actions.front();
        try {
            const TaskId task_id{static_cast<std::uint32_t>(std::stoul(action.task_id))};
            for (const auto& turnaround : state.snapshot.turnarounds) {
                const auto task = std::ranges::find(turnaround.tasks, task_id, &ServiceTaskSnapshot::id);
                if (task != turnaround.tasks.end() && task->assigned_vehicle)
                    return vehicle_outage_revealed(state, *task->assigned_vehicle);
            }
        } catch (...) { }
        return false;
    };
    const auto stand_relevant = [&](const RecoveryCandidate& candidate) {
        if (candidate.actions.empty() || candidate.actions.front().kind != RecoveryActionKind::ReassignStand)
            return false;
        return std::ranges::any_of(state.revealed_disruptions, [&](const auto& disruption) {
            return disruption.kind == "stand_closure";
        });
    };
    const auto resequence_relevance = [&](const RecoveryCandidate& candidate) {
        if (candidate.actions.empty() || candidate.actions.front().kind != RecoveryActionKind::ResequenceTasks)
            return 0;
        const auto& action = candidate.actions.front();
        const bool slowed_task = std::ranges::any_of(state.revealed_disruptions, [&](const auto& disruption) {
            return disruption.revealed_at <= state.snapshot.simulation_time &&
                (disruption.kind == "task_slowdown" || disruption.kind == "task_duration_slowdown") &&
                (disruption.entity == action.task_id || disruption.entity == action.target_id);
        });
        bool late = false;
        for (const auto& turnaround : state.snapshot.turnarounds) {
            const auto involved = [&](const std::string& id) {
                return std::ranges::any_of(turnaround.tasks, [&](const auto& task) {
                    return id_text(task.id.value()) == id;
                });
            };
            if ((involved(action.task_id) || involved(action.target_id)) && turnaround.predicted_late) late = true;
        }
        return slowed_task ? 2 : late ? 1 : 0;
    };
    const auto hold_relevance = [&](const RecoveryCandidate& candidate) {
        if (candidate.actions.empty() || candidate.actions.front().kind != RecoveryActionKind::ControlledHold) return false;
        return std::ranges::any_of(state.revealed_disruptions, [&](const auto& disruption) {
            return disruption.revealed_at <= state.snapshot.simulation_time && disruption.kind == "surface_closure";
        });
    };
    const auto priority = [&](const RecoveryCandidate& candidate) {
        if (outage_for_candidate(candidate)) return 0;
        if (stand_relevant(candidate)) return 1;
        const auto relevant = resequence_relevance(candidate);
        if (relevant > 0) return 2;
        if (hold_relevance(candidate)) return 3;
        const bool is_resequence = !candidate.actions.empty() &&
            candidate.actions.front().kind == RecoveryActionKind::ResequenceTasks;
        return is_resequence ? 5 : 4;
    };
    std::ranges::sort(executable, [&](const auto& lhs, const auto& rhs) {
        const auto left_priority = priority(lhs);
        const auto right_priority = priority(rhs);
        if (left_priority != right_priority) return left_priority < right_priority;
        const auto left_relevance = resequence_relevance(lhs);
        const auto right_relevance = resequence_relevance(rhs);
        if (left_relevance != right_relevance) return left_relevance > right_relevance;
        return lhs.id < rhs.id;
    });
    for (auto& candidate : executable) {
        if (result.size() >= config.max_candidates) break;
        result.push_back(std::move(candidate));
    }
    if (config.max_search_tier >= 2 && config.max_plan_depth >= 2) {
        const auto singles_end = result.size();
        for (std::size_t i = 1; i < singles_end && result.size() < config.max_candidates; ++i) {
            for (std::size_t j = i + 1; j < singles_end && result.size() < config.max_candidates; ++j) {
                const auto& left = result[i];
                const auto& right = result[j];
                if (!left.actions.empty() && !right.actions.empty() &&
                    left.actions.front().task_id == right.actions.front().task_id) continue;
                result.push_back({left.id + "+" + right.id, 2,
                    {left.actions.front(), right.actions.front()}});
            }
        }
    }
    return result;
}

RecoveryDecision plan_recovery(const RecoveryState& state, const RecoverySearchConfig& config,
    const std::function<RecoveryEvaluation(const RecoveryCandidate&)>& evaluate) {
    RecoveryDecision decision;
    decision.triage = triage_recovery_state(state);
    decision.fingerprint = recovery_fingerprint(state);
    decision.candidates = generate_recovery_candidates(state, config);
    if (decision.triage == RecoveryTriage::NoImpact || decision.triage == RecoveryTriage::Monitor) {
        decision.reason = "triage does not request broad recovery planning";
        decision.candidates.resize(1);
    }
    if (decision.candidates.empty()) decision.candidates.push_back({"current_plan", 0, {}});
    const auto baseline = evaluate(decision.candidates.front());
    decision.evaluations.push_back(baseline);
    if (!baseline.valid) {
        decision.reason = "baseline evaluation invalid: " + baseline.reason;
        return decision;
    }
    std::size_t best_index = 0;
    for (std::size_t i = 1; i < decision.candidates.size(); ++i) {
        decision.evaluations.push_back(evaluate(decision.candidates[i]));
        const auto& candidate = decision.evaluations.back();
        if (candidate.valid && evaluation_better(candidate.outcome, decision.evaluations[best_index].outcome))
            best_index = i;
    }
    if (best_index != 0 &&
        material_improvement(decision.evaluations[best_index].outcome, baseline.outcome, config)) {
        decision.selected_candidate_id = decision.evaluations[best_index].candidate.id;
        decision.reason = "selected materially better valid outcome";
    } else {
        decision.reason = "current plan retained; no material improvement";
    }
    return decision;
}
}  // namespace airside
