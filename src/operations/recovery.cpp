#include "airside/operations/recovery.hpp"

#include <algorithm>
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
    for (const auto& disruption : state.revealed_disruptions) {
        if (disruption.revealed_at > state.snapshot.simulation_time) continue;
        relevant = true;
        if (disruption.kind == "vehicle_outage" || disruption.kind == "stand_closure" ||
            disruption.kind == "surface_closure" || disruption.kind == "late_inbound" ||
            disruption.kind == "task_slowdown") threatened = true;
    }
    if (!relevant || !threatened) return RecoveryTriage::NoImpact;
    for (const auto& turnaround : state.snapshot.turnarounds) {
        if (turnaround.predicted_late)
            return RecoveryTriage::IntegratedPlanningRequired;
        for (const auto& task : turnaround.tasks) {
            if (task.status == TaskStatus::InProgress || task.status == TaskStatus::Waiting)
                return RecoveryTriage::LocalRecoveryAvailable;
        }
    }
    return RecoveryTriage::Monitor;
}
}  // namespace airside
