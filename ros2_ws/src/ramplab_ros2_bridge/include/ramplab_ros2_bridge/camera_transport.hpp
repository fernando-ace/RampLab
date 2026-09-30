#pragma once

#include <sensor_msgs/msg/camera_info.hpp>
#include <sensor_msgs/msg/image.hpp>
#include <sensor_msgs/msg/laser_scan.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ramplab_ros2_bridge {

struct CameraFrame {
  std::uint64_t timestamp_ns{};
  std::uint64_t sequence{};
  std::uint32_t width{};
  std::uint32_t height{};
  float horizontal_fov_degrees{};
  std::vector<std::uint8_t> bgra8;
};

struct CameraMessages {
  sensor_msgs::msg::Image image;
  sensor_msgs::msg::CameraInfo info;
};

struct LidarFrame {
  std::uint64_t timestamp_ns{};
  std::uint64_t sequence{};
  float angle_min_rad{};
  float angle_increment_rad{};
  float range_min_m{};
  float range_max_m{};
  std::vector<float> ranges_m;
};

[[nodiscard]] CameraMessages to_camera_messages(const CameraFrame& frame);
[[nodiscard]] sensor_msgs::msg::LaserScan to_laser_scan(
    const LidarFrame& frame, double scan_period_s, const std::string& frame_id);

// Receives the local little-endian RLSN v1 TCP stream sent by the Unreal
// camera. poll() is non-blocking and retains only the newest complete frame.
class CameraTcpReceiver {
public:
  explicit CameraTcpReceiver(std::uint16_t port = 39010);
  ~CameraTcpReceiver();
  CameraTcpReceiver(const CameraTcpReceiver&) = delete;
  CameraTcpReceiver& operator=(const CameraTcpReceiver&) = delete;
  [[nodiscard]] std::optional<CameraFrame> poll();
private:
  struct Impl;
  Impl* impl_{};
};

// Receives the local little-endian RLLD v1 TCP stream sent by Unreal LiDAR.
class LidarTcpReceiver {
public:
  explicit LidarTcpReceiver(std::uint16_t port = 39011);
  ~LidarTcpReceiver();
  LidarTcpReceiver(const LidarTcpReceiver&) = delete;
  LidarTcpReceiver& operator=(const LidarTcpReceiver&) = delete;
  [[nodiscard]] std::optional<LidarFrame> poll();
private:
  struct Impl;
  Impl* impl_{};
};

}  // namespace ramplab_ros2_bridge
