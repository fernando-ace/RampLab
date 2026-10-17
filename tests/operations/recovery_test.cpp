#include "airside/operations/recovery.hpp"

#include <gtest/gtest.h>

namespace airside {
namespace {

RecoveryState recovery_fixture() {
    RecoveryState state;
    state.snapshot.simulation_time = SimTime{10};
    AircraftSnapshot aircraft{};
    aircraft.id = AircraftId{1}; aircraft.state = AircraftState::AtGate;
    aircraft.assigned_gate = GateId{1}; aircraft.scheduled_arrival = SimTime{0};
    aircraft.scheduled_departure = SimTime{600};
    state.snapshot.aircraft.push_back(aircraft);
    TurnaroundSnapshot turn{};
    turn.aircraft = AircraftId{1}; turn.state = TurnaroundState::Servicing;
    turn.schedule_slack = SimTime{20}; turn.estimated_ready_time = SimTime{580};
    ServiceTaskSnapshot task{};
    task.id = TaskId{1}; task.type = ServiceType::Fueling; task.status = TaskStatus::Pending;
    turn.tasks.push_back(task); state.snapshot.turnarounds.push_back(turn);
    ServiceVehicleSnapshot vehicle{};
    vehicle.id = VehicleId{1}; vehicle.type = ServiceType::Fueling; vehicle.state = VehicleState::Idle;
    vehicle.current_node = NodeId{1}; state.snapshot.vehicles.push_back(vehicle);
    state.revealed_disruptions.push_back({"vehicle_outage", "v0", SimTime{10}, SimTime{0}});
    return state;
}

TEST(RecoveryTest, TriageSkipsBroadPlanningWithoutRevealedDisruption) {
    auto state = recovery_fixture();
    state.revealed_disruptions.clear();
    EXPECT_EQ(triage_recovery(state), RecoveryTriage::NoImpact);
}

TEST(RecoveryTest, FingerprintTracksDecisionStateButNotDiagnostics) {
    auto first = recovery_fixture();
    auto second = first;
    second.snapshot.aircraft.front().taxi_distance_m = 999.0;
    EXPECT_EQ(recovery_fingerprint(first), recovery_fingerprint(second));
    second.snapshot.simulation_time += SimTime{1};
    EXPECT_NE(recovery_fingerprint(first), recovery_fingerprint(second));
}

TEST(RecoveryTest, GeneratesSafeCompatibleVehicleReassignmentDeterministically) {
    const auto state = recovery_fixture();
    const auto first = generate_recovery_candidates(state);
    const auto second = generate_recovery_candidates(state);
    EXPECT_EQ(first, second);
    ASSERT_GE(first.size(), 2U);
    EXPECT_EQ(first.front().label, "current_plan");
    const auto gse = std::ranges::find(first, std::string{"gse_reassignment"}, &RecoveryCandidate::label);
    ASSERT_NE(gse, first.end());
    EXPECT_TRUE(recovery_action_is_safe(state, gse->actions.front()));
}

TEST(RecoveryTest, KeepsBaselineWhenCandidatesAreNotMaterial) {
    const auto state = recovery_fixture();
    const auto decision = plan_recovery(state, {}, [](const RecoveryCandidate&) {
        return RecoveryOutcome{2, 0, 1, 0, 80, 50, 20, 100.0, 40.0, 0};
    });
    EXPECT_EQ(decision.selected.label, "current_plan");
    EXPECT_EQ(decision.reason, "no_materially_better_safe_plan");
    EXPECT_EQ(decision.evaluations.front().candidate.tier, 0U);
}

TEST(RecoveryTest, MaterialCompletionImprovementSelectsTypedAction) {
    const auto state = recovery_fixture();
    const auto decision = plan_recovery(state, {}, [](const RecoveryCandidate& candidate) {
        if (candidate.tier == 0) return RecoveryOutcome{0, 1, 0, 1, 0, 0, 0, 0.0, 0.0, 0};
        if (!candidate.actions.empty() && candidate.actions.front().kind == RecoveryActionKind::ReassignVehicle)
            return RecoveryOutcome{1, 0, 1, 0, 120, 120, 5, 12.0, 0.0, candidate.actions.size()};
        return RecoveryOutcome{0, 1, 0, 1, 0, 0, 0, 0.0, 0.0, candidate.actions.size()};
    });
    EXPECT_EQ(decision.selected.label, "gse_reassignment");
    EXPECT_EQ(decision.reason, "selected_material_safe_improvement");
}

TEST(RecoveryTest, ScheduledAircraftMayOnlyMoveToUnoccupiedAvailableStand) {
    auto state = recovery_fixture();
    state.snapshot.aircraft.front().state = AircraftState::Scheduled;
    RecoveryAction action; action.kind = RecoveryActionKind::ReassignStand;
    action.aircraft = AircraftId{1}; action.gate = GateId{2};
    state.snapshot.gates.push_back({GateId{2}, "free", NodeId{2}, {}, std::nullopt, true});
    EXPECT_TRUE(recovery_action_is_safe(state, action));
    state.snapshot.aircraft.front().state = AircraftState::Arriving;
    EXPECT_FALSE(recovery_action_is_safe(state, action));
}

} // namespace
} // namespace airside
