#pragma once

#include "airside/integration/snapshot.hpp"

#include <optional>
#include <span>

namespace airside::visualization {

struct Point2 {
    double x{0.0};
    double y{0.0};
    auto operator<=>(const Point2&) const = default;
};

struct CoordinateTransform {
    double units_per_meter{100.0};
    Point2 origin{};
    bool invert_y{false};

    [[nodiscard]] constexpr Point2 apply(Vec2 meters) const noexcept {
        const double y_sign = invert_y ? -1.0 : 1.0;
        return {
            origin.x + meters.x_m * units_per_meter,
            origin.y + meters.y_m * units_per_meter * y_sign,
        };
    }
};

struct JourneySample {
    Point2 position_m;
    Point2 direction;
    std::size_t segment_index{0};
    double segment_progress{0.0};
};

[[nodiscard]] std::optional<JourneySample> sample_journey(
    const VehicleJourneySnapshot& journey,
    std::span<const RoadNodeSnapshot> nodes,
    SimTime playback_time) noexcept;

}  // namespace airside::visualization
