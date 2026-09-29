#include "airside/autonomy/sensor_model.hpp"

#include <cmath>
#include <iomanip>
#include <ostream>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace airside::autonomy {
namespace {
std::string quote_json(std::string_view value) {
    std::string result{"\""};
    for (const char character : value) {
        if (character == '\\' || character == '"') result.push_back('\\');
        if (character == '\n') { result += "\\n"; continue; }
        if (character == '\r') { result += "\\r"; continue; }
        result.push_back(character);
    }
    result.push_back('"');
    return result;
}
}

SensorClock::SensorClock(std::string sensor_id, SensorTimingConfig config, std::uint64_t seed)
    : sensor_id_(std::move(sensor_id)), config_(config), random_(seed),
      next_due_s_(config.phase_s) {
    if (sensor_id_.empty() || !std::isfinite(config_.rate_hz) || config_.rate_hz <= 0.0 ||
        !std::isfinite(config_.phase_s) || config_.phase_s < 0.0 ||
        !std::isfinite(config_.latency_s) || config_.latency_s < 0.0 ||
        !std::isfinite(config_.jitter_s) || config_.jitter_s < 0.0 ||
        !std::isfinite(config_.packet_loss_probability) ||
        config_.packet_loss_probability < 0.0 || config_.packet_loss_probability > 1.0 ||
        !std::isfinite(config_.stale_after_s) || config_.stale_after_s < 0.0) {
        throw std::invalid_argument("invalid sensor timing configuration");
    }
    if (config_.jitter_s >= 1.0 / config_.rate_hz) {
        throw std::invalid_argument("sensor jitter must be less than its update interval");
    }
}

bool SensorClock::due(double simulation_time_s) {
    if (!std::isfinite(simulation_time_s) || simulation_time_s + 1e-9 < next_due_s_) return false;
    const double interval = 1.0 / config_.rate_hz;
    do {
        const double variation = jitter_(random_) * config_.jitter_s;
        next_due_s_ += interval + variation;
    } while (next_due_s_ <= simulation_time_s + 1e-9);
    ++sequence_;
    return true;
}

bool SensorClock::packet_delivered() {
    return uniform_(random_) >= config_.packet_loss_probability;
}

ObservationMetadata SensorClock::metadata(double measurement_time_s,
                                          std::string frame_id) const {
    if (!std::isfinite(measurement_time_s) || measurement_time_s < 0.0) {
        throw std::invalid_argument("sensor measurement timestamp must be finite and nonnegative");
    }
    const bool stale = config_.stale_after_s > 0.0 &&
        config_.latency_s > config_.stale_after_s;
    return {sensor_id_, measurement_time_s, measurement_time_s + config_.latency_s,
            sequence_, std::move(frame_id), stale ? SensorHealth::Stale : SensorHealth::Valid};
}

const char* to_string(SensorHealth health) noexcept {
    switch (health) {
    case SensorHealth::Valid: return "valid";
    case SensorHealth::Degraded: return "degraded";
    case SensorHealth::Invalid: return "invalid";
    case SensorHealth::Stale: return "stale";
    case SensorHealth::Dropped: return "dropped";
    }
    return "invalid";
}

void SensorStreamRecorder::record(std::string_view sensor_type,
                                  const ObservationMetadata& metadata,
                                  std::string_view payload_json) {
    std::ostringstream line;
    line.imbue(std::locale::classic());
    line << std::setprecision(17) << "{\"sensor_type\":" << quote_json(sensor_type)
         << ",\"sensor_id\":" << quote_json(metadata.sensor_id)
         << ",\"timestamp_s\":" << metadata.timestamp_s
         << ",\"delivery_time_s\":" << metadata.delivery_time_s
         << ",\"sequence\":" << metadata.sequence
         << ",\"frame_id\":" << quote_json(metadata.frame_id)
         << ",\"health\":" << quote_json(to_string(metadata.health))
         << ",\"payload\":" << payload_json << "}\n";
    const std::string bytes = line.str();
    output_ << bytes;
    if (!output_) throw std::runtime_error("failed to write sensor JSON Lines record");
    for (const unsigned char byte : bytes) {
        digest_ ^= byte;
        digest_ *= 1099511628211ULL;
    }
    ++records_;
}

} // namespace airside::autonomy
