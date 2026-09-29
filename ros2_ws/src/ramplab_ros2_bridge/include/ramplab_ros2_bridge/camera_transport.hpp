#pragma once

#include <sensor_msgs/msg/camera_info.hpp>
#include <sensor_msgs/msg/image.hpp>

#include <cstdint>
#include <optional>
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

[[nodiscard]] CameraMessages to_camera_messages(const CameraFrame& frame);

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

}  // namespace ramplab_ros2_bridge
