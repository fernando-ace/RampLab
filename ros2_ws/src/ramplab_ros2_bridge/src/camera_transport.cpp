#include "ramplab_ros2_bridge/camera_transport.hpp"

#include "ramplab_ros2_bridge/conversions.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <numbers>
#include <stdexcept>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace ramplab_ros2_bridge {
namespace {
#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;
#else
using NativeSocket = int;
constexpr NativeSocket kInvalidSocket = -1;
#endif
constexpr std::size_t kHeaderBytes = 40;
constexpr std::uint32_t kMaxDimension = 4096;
constexpr std::uint32_t kMaxPayloadBytes = 64U * 1024U * 1024U;

std::uint16_t u16(const std::uint8_t* p) {
  return static_cast<std::uint16_t>(p[0]) | static_cast<std::uint16_t>(p[1] << 8U);
}
std::uint32_t u32(const std::uint8_t* p) {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8U) |
      (static_cast<std::uint32_t>(p[2]) << 16U) | (static_cast<std::uint32_t>(p[3]) << 24U);
}
std::uint64_t u64(const std::uint8_t* p) {
  return static_cast<std::uint64_t>(u32(p)) | (static_cast<std::uint64_t>(u32(p + 4)) << 32U);
}
float f32(const std::uint8_t* p) { return std::bit_cast<float>(u32(p)); }
void close_socket(NativeSocket socket) {
#ifdef _WIN32
  if (socket != kInvalidSocket) closesocket(socket);
#else
  if (socket != kInvalidSocket) ::close(socket);
#endif
}
bool set_nonblocking(NativeSocket socket) {
#ifdef _WIN32
  u_long mode = 1;
  return ioctlsocket(socket, FIONBIO, &mode) == 0;
#else
  return fcntl(socket, F_SETFL, fcntl(socket, F_GETFL, 0) | O_NONBLOCK) == 0;
#endif
}
}  // namespace

CameraMessages to_camera_messages(const CameraFrame& frame) {
  if (frame.width == 0 || frame.height == 0 || frame.width > kMaxDimension ||
      frame.height > kMaxDimension || !std::isfinite(frame.horizontal_fov_degrees) ||
      frame.horizontal_fov_degrees <= 1.0F || frame.horizontal_fov_degrees >= 179.0F ||
      static_cast<std::uint64_t>(frame.width) * frame.height * 4U != frame.bgra8.size()) {
    throw std::invalid_argument("invalid camera frame dimensions, FOV, or BGRA payload");
  }
  CameraMessages result;
  result.image.header.stamp = to_ros_time(static_cast<double>(frame.timestamp_ns) / 1.0e9);
  result.image.header.frame_id = "camera";
  result.image.height = frame.height;
  result.image.width = frame.width;
  result.image.encoding = "bgra8";
  result.image.is_bigendian = false;
  result.image.step = frame.width * 4U;
  result.image.data = frame.bgra8;
  result.info.header = result.image.header;
  result.info.height = frame.height;
  result.info.width = frame.width;
  result.info.distortion_model = "plumb_bob";
  result.info.d.assign(5, 0.0);
  const double fx = static_cast<double>(frame.width) /
      (2.0 * std::tan(frame.horizontal_fov_degrees * std::numbers::pi / 360.0));
  const double fy = fx;
  const double cx = (static_cast<double>(frame.width) - 1.0) * 0.5;
  const double cy = (static_cast<double>(frame.height) - 1.0) * 0.5;
  result.info.k = {fx, 0.0, cx, 0.0, fy, cy, 0.0, 0.0, 1.0};
  result.info.r = {1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
  result.info.p = {fx, 0.0, cx, 0.0, 0.0, fy, cy, 0.0, 0.0, 0.0, 1.0, 0.0};
  return result;
}

sensor_msgs::msg::LaserScan to_laser_scan(
    const LidarFrame& frame, double scan_period_s, const std::string& frame_id) {
  if (frame.ranges_m.size() < 2 || !std::isfinite(scan_period_s) || scan_period_s <= 0.0 || frame_id.empty())
    throw std::invalid_argument("invalid Unreal LiDAR message metadata");
  sensor_msgs::msg::LaserScan message;
  message.header.stamp = to_ros_time(static_cast<double>(frame.timestamp_ns) / 1.0e9);
  message.header.frame_id = frame_id;
  message.angle_min = frame.angle_min_rad;
  message.angle_increment = frame.angle_increment_rad;
  message.angle_max = frame.angle_min_rad + frame.angle_increment_rad * static_cast<float>(frame.ranges_m.size() - 1);
  message.scan_time = static_cast<float>(scan_period_s);
  message.time_increment = 0.0F;
  message.range_min = frame.range_min_m;
  message.range_max = frame.range_max_m;
  message.ranges = frame.ranges_m;
  message.intensities.clear();
  return message;
}

struct CameraTcpReceiver::Impl {
  NativeSocket listener{kInvalidSocket};
  NativeSocket client{kInvalidSocket};
  std::vector<std::uint8_t> bytes;
  std::uint64_t last_sequence{};
#ifdef _WIN32
  bool winsock_started{};
#endif

  explicit Impl(std::uint16_t port) {
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) throw std::runtime_error("WSAStartup failed");
    winsock_started = true;
    listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#else
    listener = ::socket(AF_INET, SOCK_STREAM, 0);
#endif
    if (listener == kInvalidSocket) {
#ifdef _WIN32
      WSACleanup();
      winsock_started = false;
#endif
      throw std::runtime_error("camera TCP socket creation failed");
    }
    int reuse = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(listener, 1) != 0 || !set_nonblocking(listener)) {
      close_socket(listener);
      listener = kInvalidSocket;
#ifdef _WIN32
      WSACleanup();
      winsock_started = false;
#endif
      throw std::runtime_error("camera TCP listener could not bind localhost port " + std::to_string(port));
    }
  }

  ~Impl() {
    close_socket(client);
    close_socket(listener);
#ifdef _WIN32
    if (winsock_started) WSACleanup();
#endif
  }

  std::optional<CameraFrame> poll() {
    if (client == kInvalidSocket) {
      sockaddr_in peer{};
#ifdef _WIN32
      int size = sizeof(peer);
#else
      socklen_t size = sizeof(peer);
#endif
      const NativeSocket accepted = ::accept(listener, reinterpret_cast<sockaddr*>(&peer), &size);
      if (accepted != kInvalidSocket) {
        if (ntohl(peer.sin_addr.s_addr) != INADDR_LOOPBACK || !set_nonblocking(accepted)) close_socket(accepted);
        else { client = accepted; bytes.clear(); }
      }
      return std::nullopt;
    }
    std::uint8_t chunk[65536];
    for (;;) {
      const int received = static_cast<int>(::recv(client, reinterpret_cast<char*>(chunk), sizeof(chunk), 0));
      if (received > 0) bytes.insert(bytes.end(), chunk, chunk + received);
      else if (received == 0) { close_socket(client); client = kInvalidSocket; bytes.clear(); return std::nullopt; }
      else break;
    }
    std::optional<CameraFrame> latest;
    while (bytes.size() >= kHeaderBytes) {
      const auto* h = bytes.data();
      if (std::memcmp(h, "RLSN", 4) != 0 || u16(h + 4) != 1 || u16(h + 6) != 1) {
        close_socket(client); client = kInvalidSocket; bytes.clear(); return std::nullopt;
      }
      const std::uint64_t timestamp = u64(h + 8), sequence = u64(h + 16);
      const std::uint32_t width = u32(h + 24), height = u32(h + 28), payload_size = u32(h + 32);
      float fov{}; std::memcpy(&fov, h + 36, sizeof(fov));
      if (width == 0 || height == 0 || width > kMaxDimension || height > kMaxDimension ||
          static_cast<std::uint64_t>(width) * height * 4U != payload_size ||
          payload_size > kMaxPayloadBytes || !std::isfinite(fov) || fov <= 1.0F || fov >= 179.0F) {
        close_socket(client); client = kInvalidSocket; bytes.clear(); return std::nullopt;
      }
      if (bytes.size() < kHeaderBytes + payload_size) break;
      if (sequence > last_sequence) {
        CameraFrame frame{timestamp, sequence, width, height, fov, {}};
        frame.bgra8.assign(bytes.begin() + static_cast<std::ptrdiff_t>(kHeaderBytes),
            bytes.begin() + static_cast<std::ptrdiff_t>(kHeaderBytes + payload_size));
        latest = std::move(frame);
        last_sequence = sequence;
      }
      bytes.erase(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(kHeaderBytes + payload_size));
    }
    return latest;
  }
};

CameraTcpReceiver::CameraTcpReceiver(std::uint16_t port) : impl_(new Impl(port)) {}
CameraTcpReceiver::~CameraTcpReceiver() { delete impl_; }
std::optional<CameraFrame> CameraTcpReceiver::poll() { return impl_->poll(); }

struct LidarTcpReceiver::Impl {
  static constexpr std::size_t header_bytes = 48;
  NativeSocket listener{kInvalidSocket};
  NativeSocket client{kInvalidSocket};
  std::vector<std::uint8_t> bytes;
  std::uint64_t last_sequence{};
#ifdef _WIN32
  bool winsock_started{};
#endif
  explicit Impl(std::uint16_t port) {
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) throw std::runtime_error("WSAStartup failed");
    winsock_started = true;
    listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#else
    listener = ::socket(AF_INET, SOCK_STREAM, 0);
#endif
    if (listener == kInvalidSocket) {
#ifdef _WIN32
      WSACleanup();
      winsock_started = false;
#endif
      throw std::runtime_error("LiDAR TCP socket creation failed");
    }
    int reuse = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(listener, 1) != 0 || !set_nonblocking(listener)) {
      close_socket(listener); listener = kInvalidSocket;
#ifdef _WIN32
      WSACleanup(); winsock_started = false;
#endif
      throw std::runtime_error("LiDAR TCP listener could not bind localhost port " + std::to_string(port));
    }
  }
  ~Impl() {
    close_socket(client); close_socket(listener);
#ifdef _WIN32
    if (winsock_started) WSACleanup();
#endif
  }
  std::optional<LidarFrame> poll() {
    if (client == kInvalidSocket) {
      sockaddr_in peer{};
#ifdef _WIN32
      int size = sizeof(peer);
#else
      socklen_t size = sizeof(peer);
#endif
      const NativeSocket accepted = ::accept(listener, reinterpret_cast<sockaddr*>(&peer), &size);
      if (accepted != kInvalidSocket) {
        if (ntohl(peer.sin_addr.s_addr) != INADDR_LOOPBACK || !set_nonblocking(accepted)) close_socket(accepted);
        else { client = accepted; bytes.clear(); }
      }
      return std::nullopt;
    }
    std::uint8_t chunk[65536];
    for (;;) {
      const int received = static_cast<int>(::recv(client, reinterpret_cast<char*>(chunk), sizeof(chunk), 0));
      if (received > 0) bytes.insert(bytes.end(), chunk, chunk + received);
      else if (received == 0) { close_socket(client); client = kInvalidSocket; bytes.clear(); return std::nullopt; }
      else break;
    }
    std::optional<LidarFrame> latest;
    while (bytes.size() >= header_bytes) {
      const auto* h = bytes.data();
      if (std::memcmp(h, "RLLD", 4) != 0 || u16(h + 4) != 1 || u16(h + 6) != 1) {
        close_socket(client); client = kInvalidSocket; bytes.clear(); return std::nullopt;
      }
      const std::uint64_t timestamp = u64(h + 8), sequence = u64(h + 16);
      const std::uint32_t count = u32(h + 24), payload_size = u32(h + 44);
      const float angle_min = f32(h + 28), angle_increment = f32(h + 32);
      const float range_min = f32(h + 36), range_max = f32(h + 40);
      if (count < 2 || count > 4096 || payload_size != count * 4U ||
          !std::isfinite(angle_min) || !std::isfinite(angle_increment) || angle_increment <= 0.0F ||
          !std::isfinite(range_min) || !std::isfinite(range_max) || range_min < 0.0F || range_max <= range_min) {
        close_socket(client); client = kInvalidSocket; bytes.clear(); return std::nullopt;
      }
      if (bytes.size() < header_bytes + payload_size) break;
      if (sequence > last_sequence) {
        LidarFrame frame{timestamp, sequence, angle_min, angle_increment, range_min, range_max, {}};
        frame.ranges_m.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
          const float range = f32(h + header_bytes + i * 4U);
          if (!std::isfinite(range) || range < range_min || range > range_max) {
            close_socket(client); client = kInvalidSocket; bytes.clear(); return std::nullopt;
          }
          frame.ranges_m.push_back(range);
        }
        latest = std::move(frame);
        last_sequence = sequence;
      }
      bytes.erase(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(header_bytes + payload_size));
    }
    return latest;
  }
};

LidarTcpReceiver::LidarTcpReceiver(std::uint16_t port) : impl_(new Impl(port)) {}
LidarTcpReceiver::~LidarTcpReceiver() { delete impl_; }
std::optional<LidarFrame> LidarTcpReceiver::poll() { return impl_->poll(); }

}  // namespace ramplab_ros2_bridge
