#pragma once

#include <cstdint>
#include <iosfwd>
#include <random>
#include <string>
#include <string_view>

namespace airside::autonomy {

enum class SensorHealth : std::uint8_t { Valid, Degraded, Invalid, Stale, Dropped };

struct ObservationMetadata {
    std::string sensor_id;
    double timestamp_s{};
    double delivery_time_s{};
    std::uint64_t sequence{};
    std::string frame_id;
    SensorHealth health{SensorHealth::Valid};
};

template<class Payload>
struct SensorObservation {
    ObservationMetadata metadata;
    Payload payload;
};

struct SensorTimingConfig {
    double rate_hz{};
    double phase_s{};
    double latency_s{};
    double jitter_s{};
    double packet_loss_probability{};
    double stale_after_s{};
};

// A per-sensor clock. Each sensor owns its PRNG, so adding a different sensor
// does not change this sensor's schedule or packet-loss sequence.
class SensorClock {
public:
    SensorClock(std::string sensor_id, SensorTimingConfig config, std::uint64_t seed);
    [[nodiscard]] bool due(double simulation_time_s);
    [[nodiscard]] double next_due_s() const noexcept { return next_due_s_; }
    [[nodiscard]] std::uint64_t sequence() const noexcept { return sequence_; }
    [[nodiscard]] bool packet_delivered();
    [[nodiscard]] ObservationMetadata metadata(double measurement_time_s,
                                                std::string frame_id) const;
    [[nodiscard]] const SensorTimingConfig& config() const noexcept { return config_; }
private:
    std::string sensor_id_;
    SensorTimingConfig config_;
    std::mt19937_64 random_;
    std::uniform_real_distribution<double> uniform_{0.0, 1.0};
    std::uniform_real_distribution<double> jitter_{-1.0, 1.0};
    double next_due_s_{};
    std::uint64_t sequence_{};
};

[[nodiscard]] const char* to_string(SensorHealth health) noexcept;

class SensorStreamRecorder {
public:
    explicit SensorStreamRecorder(std::ostream& output) noexcept : output_(output) {}
    void record(std::string_view sensor_type, const ObservationMetadata& metadata,
                std::string_view payload_json);
    [[nodiscard]] std::uint64_t digest() const noexcept { return digest_; }
    [[nodiscard]] std::uint64_t records() const noexcept { return records_; }
private:
    std::ostream& output_;
    std::uint64_t digest_{14695981039346656037ULL};
    std::uint64_t records_{};
};

} // namespace airside::autonomy
