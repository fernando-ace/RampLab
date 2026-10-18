#include "airside/operations/recovery.hpp"

#include <algorithm>
#include <gtest/gtest.h>

namespace airside {

TEST(RecoveryCoreTest, ParsesPublicModeNamesAndDefaultsRemainReactiveAtCaller) {
    EXPECT_EQ(recovery_mode_from_string("REACTIVE"), RecoveryMode::Reactive);
    EXPECT_EQ(recovery_mode_from_string("RECOVERY_SHADOW"), RecoveryMode::Shadow);
    EXPECT_EQ(recovery_mode_from_string("INTEGRATED_RECOVERY"), RecoveryMode::Integrated);
    EXPECT_STREQ(to_string(RecoveryMode::Reactive), "REACTIVE");
    EXPECT_THROW(recovery_mode_from_string("GOAL50"), std::invalid_argument);
}

TEST(RecoveryCoreTest, FingerprintIsStableAndTracksDecisionFields) {
    RecoveryState state;
    state.snapshot.simulation_time = SimTime{30};
    state.snapshot.vehicles.push_back({VehicleId{2}, "diagnostic name", ServiceType::Fueling,
        VehicleState::Idle, NodeId{4}, NodeId{4}});
    const auto first = recovery_fingerprint(state);
    EXPECT_EQ(first, recovery_fingerprint(state));
    state.snapshot.vehicles.front().current_node = NodeId{5};
    EXPECT_NE(first, recovery_fingerprint(state));
}

TEST(RecoveryCoreTest, FutureDisruptionsAreIgnoredAtEarlierDecisionBoundary) {
    RecoveryState state;
    state.snapshot.simulation_time = SimTime{10};
    state.revealed_disruptions.push_back({SimTime{20}, "vehicle_outage", "vehicle:1", "future"});
    const auto baseline = recovery_fingerprint(state);
    auto alternate = state;
    alternate.revealed_disruptions.front().entity = "vehicle:99";
    EXPECT_EQ(baseline, recovery_fingerprint(alternate));
    EXPECT_EQ(RecoveryTriage::NoImpact, triage_recovery_state(state));
}

TEST(RecoveryPlannerTest, GeneratesStableTierZeroAndEligibleVehicleCandidates) {
    RecoveryState state;
    state.snapshot.simulation_time = SimTime{10};
    TurnaroundSnapshot turnaround;
    turnaround.aircraft = AircraftId{1};
    turnaround.predicted_late = true;
    ServiceTaskSnapshot task;
    task.id = TaskId{8}; task.type = ServiceType::Fueling; task.status = TaskStatus::Waiting;
    turnaround.tasks.push_back(task);
    state.snapshot.turnarounds.push_back(turnaround);
    state.snapshot.vehicles.push_back({VehicleId{3}, "backup", ServiceType::Fueling,
        VehicleState::Idle, NodeId{2}, NodeId{2}});
    RecoverySearchConfig config;
    const auto first = generate_recovery_candidates(state, config);
    const auto second = generate_recovery_candidates(state, config);
    ASSERT_FALSE(first.empty());
    EXPECT_EQ(first.front().id, "current_plan");
    EXPECT_EQ(first, second);
    EXPECT_TRUE(std::ranges::any_of(first, [](const auto& candidate) {
        return candidate.id == "vehicle:8:3";
    }));
}

TEST(RecoveryPlannerTest, EvaluatesBaselineFirstAndRequiresMaterialImprovement) {
    RecoveryState state;
    state.snapshot.simulation_time = SimTime{10};
    state.revealed_disruptions.push_back({SimTime{1}, "vehicle_outage", "vehicle:1", "outage"});
    TurnaroundSnapshot turnaround;
    turnaround.aircraft = AircraftId{1}; turnaround.predicted_late = true;
    ServiceTaskSnapshot task;
    task.id = TaskId{3}; task.type = ServiceType::Fueling; task.status = TaskStatus::Waiting;
    turnaround.tasks.push_back(task);
    state.snapshot.turnarounds.push_back(turnaround);
    ServiceVehicleSnapshot backup;
    backup.id = VehicleId{3}; backup.name = "backup"; backup.type = ServiceType::Fueling;
    backup.state = VehicleState::Idle; backup.current_node = NodeId{1}; backup.destination_node = NodeId{1};
    state.snapshot.vehicles.push_back(backup);
    RecoverySearchConfig config;
    config.max_candidates = 2;
    std::vector<std::string> evaluation_order;
    const auto decision = plan_recovery(state, config, [&](const RecoveryCandidate& candidate) {
        evaluation_order.push_back(candidate.id);
        RecoveryEvaluation result;
        result.candidate = candidate; result.valid = true;
        result.outcome.worst_departure_delay_seconds = candidate.tier == 0 ? 600.0 : 100.0;
        return result;
    });
    ASSERT_FALSE(evaluation_order.empty());
    EXPECT_EQ(evaluation_order.front(), "current_plan");
    EXPECT_TRUE(decision.selected_candidate_id.has_value());
}

TEST(RecoveryPlannerTest, StrandedAssignedTaskSurvivesCandidateCapAheadOfDiagnostics) {
    RecoveryState state;
    state.snapshot.simulation_time = SimTime{30};
    state.revealed_disruptions.push_back({SimTime{20}, "vehicle_outage", "vehicle:1", "failed"});
    TurnaroundSnapshot turnaround;
    turnaround.aircraft = AircraftId{7};
    turnaround.predicted_late = true;
    ServiceTaskSnapshot task;
    task.id = TaskId{8}; task.type = ServiceType::Fueling; task.status = TaskStatus::InProgress;
    task.assigned_vehicle = VehicleId{1};
    turnaround.tasks.push_back(task);
    state.snapshot.turnarounds.push_back(turnaround);
    ServiceVehicleSnapshot failed;
    failed.id = VehicleId{1}; failed.name = "failed"; failed.type = ServiceType::Fueling;
    failed.state = VehicleState::Assigned; failed.current_node = NodeId{2}; failed.destination_node = NodeId{2};
    failed.fleet_status = "Unavailable";
    state.snapshot.vehicles.push_back(failed);
    ServiceVehicleSnapshot backup;
    backup.id = VehicleId{2}; backup.name = "backup"; backup.type = ServiceType::Fueling;
    backup.state = VehicleState::Idle; backup.current_node = NodeId{3}; backup.destination_node = NodeId{3};
    state.snapshot.vehicles.push_back(backup);
    for (std::uint32_t i = 10; i < 20; ++i) {
        state.snapshot.gates.push_back({GateId{i}, "stand", NodeId{i}, {}, {}, true});
    }
    AircraftSnapshot aircraft;
    aircraft.id = AircraftId{7};
    aircraft.flight_number = "synthetic";
    aircraft.state = AircraftState::AtGate;
    aircraft.assigned_gate = GateId{1};
    state.snapshot.aircraft.push_back(aircraft);
    RecoverySearchConfig config;
    config.max_candidates = 2;
    config.max_search_tier = 1;
    const auto first = generate_recovery_candidates(state, config);
    const auto second = generate_recovery_candidates(state, config);
    ASSERT_EQ(first.size(), 2U);
    EXPECT_EQ(first, second);
    EXPECT_EQ(first[1].id, "vehicle:8:2");
    ASSERT_EQ(first[1].actions.size(), 1U);
    EXPECT_EQ(first[1].actions.front().kind, RecoveryActionKind::ReassignVehicle);
    EXPECT_NE(first[1].actions.front().reason.find("stranded"), std::string::npos);
}

}  // namespace airside
