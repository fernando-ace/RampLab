#include "airside/operations/recovery.hpp"

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

}  // namespace airside
