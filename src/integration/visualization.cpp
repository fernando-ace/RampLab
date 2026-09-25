#include "airside/integration/visualization.hpp"

#include <algorithm>
#include <cmath>

namespace airside::visualization {
namespace {

const RoadNodeSnapshot* find_node(
    std::span<const RoadNodeSnapshot> nodes,
    NodeId id) noexcept {
    const auto match = std::ranges::find(nodes, id, &RoadNodeSnapshot::id);
    return match == nodes.end() ? nullptr : &*match;
}

}  // namespace

std::optional<JourneySample> sample_journey(
    const VehicleJourneySnapshot& journey,
    std::span<const RoadNodeSnapshot> nodes,
    SimTime playback_time) noexcept {
    if (journey.segments.empty()) return std::nullopt;

    std::size_t index = 0;
    if (playback_time >= journey.expected_arrival_time) {
        index = journey.segments.size() - 1;
    } else {
        const auto match = std::ranges::find_if(journey.segments, [playback_time](const auto& segment) {
            return playback_time < segment.expected_arrival_time;
        });
        index = match == journey.segments.end()
            ? journey.segments.size() - 1
            : static_cast<std::size_t>(std::distance(journey.segments.begin(), match));
    }

    const auto& segment = journey.segments[index];
    const auto* from = find_node(nodes, segment.from);
    const auto* to = find_node(nodes, segment.to);
    if (from == nullptr || to == nullptr) return std::nullopt;

    const auto duration = segment.expected_arrival_time - segment.departure_time;
    const double progress = duration <= SimTime::zero()
        ? 1.0
        : std::clamp(
              static_cast<double>((playback_time - segment.departure_time).count()) /
                  static_cast<double>(duration.count()),
              0.0,
              1.0);

    const double delta_x = to->position_m.x_m - from->position_m.x_m;
    const double delta_y = to->position_m.y_m - from->position_m.y_m;
    const double length = std::hypot(delta_x, delta_y);

    return JourneySample{
        .position_m = {
            std::lerp(from->position_m.x_m, to->position_m.x_m, progress),
            std::lerp(from->position_m.y_m, to->position_m.y_m, progress),
        },
        .direction = length > 0.0
            ? Point2{delta_x / length, delta_y / length}
            : Point2{1.0, 0.0},
        .segment_index = index,
        .segment_progress = progress,
    };
}

}  // namespace airside::visualization
