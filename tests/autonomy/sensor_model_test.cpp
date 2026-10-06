#include "airside/autonomy/sensor_model.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <sstream>
#include <string>
#include <vector>

using namespace airside::autonomy;

TEST(SensorClockTest, PhaseAndSequencesUseSimulationTime) {
    SensorClock clock{"gnss", {10.0, 0.15, 0.02, 0.0, 0.0, 0.5}, 42};
    EXPECT_FALSE(clock.due(0.14));
    EXPECT_TRUE(clock.due(0.15));
    EXPECT_EQ(clock.sequence(), 1U);
    const auto metadata = clock.metadata(0.15, "gnss_link");
    EXPECT_EQ(metadata.sensor_id, "gnss");
    EXPECT_EQ(metadata.frame_id, "gnss_link");
    EXPECT_DOUBLE_EQ(metadata.timestamp_s, 0.15);
    EXPECT_DOUBLE_EQ(metadata.delivery_time_s, 0.17);
    EXPECT_EQ(metadata.health, SensorHealth::Valid);
    EXPECT_FALSE(clock.due(0.24));
    EXPECT_TRUE(clock.due(0.25));
    EXPECT_EQ(clock.sequence(), 2U);
}

TEST(SensorClockTest, SeededJitterAndPacketLossRepeatExactly) {
    const auto execute = [] {
        SensorClock clock{"lidar", {20.0, 0.0, 0.03, 0.004, 0.35, 0.0}, 7019};
        std::vector<std::pair<double, bool>> sequence;
        for (double now = 0.0; now < 2.0; now += 0.005) {
            if (clock.due(now)) sequence.emplace_back(now, clock.packet_delivered());
        }
        return sequence;
    };
    EXPECT_EQ(execute(), execute());
    EXPECT_FALSE(execute().empty());
}

TEST(SensorClockTest, InvalidAndStaleObservationsAreExplicit) {
    EXPECT_THROW((SensorClock{"bad", {10.0, 0.0, 0.0, 0.1, 0.0, 0.0}, 1}), std::invalid_argument);
    SensorClock clock{"imu", {100.0, 0.0, 0.2, 0.0, 0.0, 0.1}, 1};
    EXPECT_EQ(clock.metadata(0.0, "imu_link").health, SensorHealth::Stale);
    EXPECT_TRUE(clock.packet_delivered());
}

TEST(SensorRecorderTest, WritesMachineReadableRecordsAndStableDigest) {
    std::ostringstream first, second;
    const ObservationMetadata metadata{"gnss", 1.25, 1.30, 7, "gnss_link", SensorHealth::Valid};
    SensorStreamRecorder a{first}, b{second};
    a.record("gnss", metadata, "{\"east_m\":2.5}");
    b.record("gnss", metadata, "{\"east_m\":2.5}");
    EXPECT_EQ(first.str(), second.str());
    EXPECT_EQ(a.digest(), b.digest());
    EXPECT_EQ(a.records(), 1U);
    EXPECT_NE(first.str().find("\"sequence\":7"), std::string::npos);
    EXPECT_NE(first.str().find("\"health\":\"valid\""), std::string::npos);
}
