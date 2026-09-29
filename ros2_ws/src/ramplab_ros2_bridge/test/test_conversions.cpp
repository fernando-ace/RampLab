#include "ramplab_ros2_bridge/conversions.hpp"
#include "ramplab_ros2_bridge/camera_transport.hpp"
#include "ramplab_ros2_bridge/command_watchdog.hpp"
#include "ramplab_ros2_common/geodesy.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>

using namespace ramplab_ros2_bridge;

TEST(Ros2Conversions, UnrealCameraFramePublishesImageAndCalibratedCameraInfo) {
  CameraFrame frame;
  frame.timestamp_ns = 1'250'000'000ULL;
  frame.sequence = 7;
  frame.width = 4;
  frame.height = 2;
  frame.horizontal_fov_degrees = 90.0F;
  frame.bgra8.assign(4U * 2U * 4U, 0x5a);
  const auto messages = to_camera_messages(frame);
  EXPECT_EQ(messages.image.header.frame_id, "camera");
  EXPECT_NEAR(from_ros_time(messages.image.header.stamp), 1.25, 1e-9);
  EXPECT_EQ(messages.image.encoding, "bgra8");
  EXPECT_EQ(messages.image.width, 4U);
  EXPECT_EQ(messages.image.height, 2U);
  EXPECT_EQ(messages.image.step, 16U);
  EXPECT_EQ(messages.image.data, frame.bgra8);
  EXPECT_EQ(messages.info.header, messages.image.header);
  EXPECT_EQ(messages.info.distortion_model, "plumb_bob");
  EXPECT_NEAR(messages.info.k[0], 2.0, 1e-12);
  EXPECT_NEAR(messages.info.k[4], 2.0, 1e-12);
  EXPECT_NEAR(messages.info.k[2], 1.5, 1e-12);
  EXPECT_NEAR(messages.info.k[5], 0.5, 1e-12);
  EXPECT_EQ(messages.info.d.size(), 5U);
}

TEST(Ros2Conversions, UnrealCameraTransportRejectsMalformedPixelBuffers) {
  CameraFrame frame;
  frame.width = 2;
  frame.height = 2;
  frame.horizontal_fov_degrees = 90.0F;
  frame.bgra8.resize(3);
  EXPECT_THROW((void)to_camera_messages(frame), std::invalid_argument);
}

TEST(Ros2Conversions, SimulationTimeRoundTripsWithNanosecondPrecision) {
  const auto stamp = to_ros_time(12.345678901);
  EXPECT_EQ(stamp.sec, 12);
  EXPECT_EQ(stamp.nanosec, 345678901U);
  EXPECT_NEAR(from_ros_time(stamp), 12.345678901, 1e-9);
  EXPECT_THROW((void)to_ros_time(-0.1), std::invalid_argument);
  EXPECT_THROW((void)to_ros_time(std::numeric_limits<double>::infinity()), std::invalid_argument);
}

TEST(Ros2Conversions, HeadingUsesRep103CounterClockwiseYaw) {
  const auto q = yaw_quaternion(std::numbers::pi / 2.0);
  EXPECT_NEAR(q.z, std::sqrt(0.5), 1e-12);
  EXPECT_NEAR(q.w, std::sqrt(0.5), 1e-12);
}

TEST(Ros2Conversions, LaserScanPreservesRampLabBeamOrderingAndMetadata) {
  airside::autonomy::LidarScan scan{1.0, -0.5, 0.25, 0.1, 20.0, {2.0, 3.0, 4.0, 5.0, 6.0}};
  const auto message = to_laser_scan(scan, 0.1, "lidar");
  EXPECT_EQ(message.header.frame_id, "lidar");
  EXPECT_NEAR(from_ros_time(message.header.stamp), 1.0, 1e-9);
  EXPECT_FLOAT_EQ(message.angle_min, -0.5F);
  EXPECT_FLOAT_EQ(message.angle_max, 0.5F);
  EXPECT_FLOAT_EQ(message.angle_increment, 0.25F);
  EXPECT_FLOAT_EQ(message.time_increment, 0.0F);
  EXPECT_FLOAT_EQ(message.scan_time, 0.1F);
  EXPECT_FLOAT_EQ(message.range_min, 0.1F);
  EXPECT_FLOAT_EQ(message.range_max, 20.0F);
  ASSERT_EQ(message.ranges.size(), 5U);
  EXPECT_FLOAT_EQ(message.ranges.front(), 2.0F);
  EXPECT_FLOAT_EQ(message.ranges.back(), 6.0F);
  EXPECT_TRUE(message.intensities.empty());
}

TEST(Ros2Conversions, ImuPublishesMeasuredPlanarYawRateAndAcceleration) {
  airside::autonomy::SensorConfig config;
  airside::autonomy::ImuMeasurement sample{2.0, 0.4, 0.2, 0.5};
  const auto message = to_imu(sample, config, "imu");
  EXPECT_EQ(message.header.frame_id, "imu");
  EXPECT_NEAR(message.angular_velocity.z, 0.2, 1e-12);
  EXPECT_NEAR(message.linear_acceleration.x, 0.5, 1e-12);
  EXPECT_NEAR(message.orientation.z, std::sin(0.2), 1e-12);
  EXPECT_NEAR(message.orientation.w, std::cos(0.2), 1e-12);
  EXPECT_GT(message.orientation_covariance[8], 0.0);
  EXPECT_GT(message.angular_velocity_covariance[8], 0.0);
  EXPECT_GT(message.linear_acceleration_covariance[0], 0.0);
}

TEST(Ros2Conversions, GnssUsesWgs84AndDiagonalHorizontalVariance) {
  airside::autonomy::GnssMeasurement sample{3.0, {240.0, 50.0}, 0.98};
  const auto message = to_nav_sat_fix(sample, "gnss");
  EXPECT_EQ(message.header.frame_id, "gnss");
  EXPECT_EQ(message.status.status, sensor_msgs::msg::NavSatStatus::STATUS_FIX);
  EXPECT_EQ(message.status.service, sensor_msgs::msg::NavSatStatus::SERVICE_GPS);
  EXPECT_EQ(message.position_covariance_type, sensor_msgs::msg::NavSatFix::COVARIANCE_TYPE_DIAGONAL_KNOWN);
  EXPECT_NEAR(message.position_covariance[0], 0.25, 1e-12);
  EXPECT_NEAR(message.position_covariance[4], 0.25, 1e-12);
  const auto local = ramplab_ros2_common::geodetic_to_local(
      {message.latitude, message.longitude, message.altitude});
  EXPECT_NEAR(local.east_m, 240.0, 1e-6);
  EXPECT_NEAR(local.north_m, 50.0, 1e-6);
}

TEST(Ros2Conversions, OdometryUsesOdomAndBaseLinkFrames) {
  const auto message = to_odometry(4.0, {12.0, 30.0}, 0.25, 2.0, -0.1);
  EXPECT_EQ(message.header.frame_id, "odom");
  EXPECT_EQ(message.child_frame_id, "base_link");
  EXPECT_DOUBLE_EQ(message.pose.pose.position.x, 12.0);
  EXPECT_DOUBLE_EQ(message.pose.pose.position.y, 30.0);
  EXPECT_DOUBLE_EQ(message.twist.twist.linear.x, 2.0);
  EXPECT_DOUBLE_EQ(message.twist.twist.angular.z, -0.1);
}

TEST(Ros2Conversions, FilteredOdometryMapsCoreCovarianceIntoRosPoseAndTwist) {
  airside::autonomy::EstimatedState e;
  e.timestamp_s=5.0;e.position={4.0,-2.0};e.heading_rad=0.3;e.speed_mps=1.7;
  e.covariance[0]=0.4;e.covariance[1]=0.03;e.covariance[2]=0.01;
  e.covariance[4]=0.03;e.covariance[5]=0.5;e.covariance[6]=0.02;
  e.covariance[8]=0.01;e.covariance[9]=0.02;e.covariance[10]=0.06;e.covariance[15]=0.2;
  e.heading_uncertainty_rad=std::sqrt(0.06);
  const auto message=to_filtered_odometry(e);
  EXPECT_NEAR(from_ros_time(message.header.stamp),5.0,1e-9);
  EXPECT_DOUBLE_EQ(message.pose.covariance[0],0.4);
  EXPECT_DOUBLE_EQ(message.pose.covariance[1],0.03);
  EXPECT_DOUBLE_EQ(message.pose.covariance[5],0.01);
  EXPECT_DOUBLE_EQ(message.pose.covariance[7],0.5);
  EXPECT_DOUBLE_EQ(message.pose.covariance[35],0.06);
  EXPECT_DOUBLE_EQ(message.twist.covariance[0],0.2);
}

TEST(Ros2Conversions, TwistRejectsNonFiniteAndClampsPhysicalLimits) {
  airside::autonomy::VehicleLimits limits;
  geometry_msgs::msg::Twist input;
  input.linear.x = 7.0;
  input.angular.z = -2.0;
  const auto clamped = from_twist(input, limits);
  ASSERT_TRUE(clamped);
  EXPECT_DOUBLE_EQ(clamped->target_speed_mps, limits.maximum_speed_mps);
  EXPECT_DOUBLE_EQ(clamped->target_yaw_rate_radps, -limits.maximum_yaw_rate_radps);
  input.linear.x = -1.0;
  input.angular.z = 0.2;
  const auto reverse = from_twist(input, limits);
  ASSERT_TRUE(reverse);
  EXPECT_DOUBLE_EQ(reverse->target_speed_mps, 0.0);
  input.linear.y = std::numeric_limits<double>::quiet_NaN();
  EXPECT_FALSE(from_twist(input, limits));
  input.linear.y = 0.0;
  input.angular.x = std::numeric_limits<double>::infinity();
  EXPECT_FALSE(from_twist(input, limits));
}

TEST(CommandWatchdog, UsesSimulationTimeAndLatchesTimeoutUntilFreshCommand) {
  airside::autonomy::VehicleCommand initial{2.0, 0.1};
  CommandWatchdog watchdog(0.5, 10.0);
  EXPECT_FALSE(watchdog.accept(initial, 10.0));
  EXPECT_FALSE(watchdog.poll(10.5).timed_out);
  const auto expired = watchdog.poll(10.52);
  EXPECT_TRUE(expired.timed_out);
  EXPECT_TRUE(expired.timeout_activated);
  EXPECT_DOUBLE_EQ(expired.command.target_speed_mps, 0.0);
  EXPECT_DOUBLE_EQ(expired.command.target_yaw_rate_radps, 0.0);
  EXPECT_FALSE(watchdog.poll(11.0).timeout_activated);
  EXPECT_EQ(watchdog.timeout_count(), 1U);
  EXPECT_TRUE(watchdog.accept({1.0, -0.2}, 11.1));
  const auto fresh = watchdog.poll(11.2);
  EXPECT_FALSE(fresh.timed_out);
  EXPECT_DOUBLE_EQ(fresh.command.target_speed_mps, 1.0);
  EXPECT_FALSE(watchdog.accept({4.0, 0.0}, 11.0));
}

TEST(CommandWatchdog, StopsIfNoInitialControllerAppears) {
  CommandWatchdog watchdog(0.25, 0.0);
  EXPECT_DOUBLE_EQ(watchdog.poll(0.2).command.target_speed_mps, 0.0);
  EXPECT_TRUE(watchdog.poll(0.26).timed_out);
  EXPECT_EQ(watchdog.timeout_count(), 1U);
}
