#pragma once

namespace airside::surface_safety {

struct SeparationAssessment {
    bool hold{};
    bool collision{};
};

[[nodiscard]] constexpr SeparationAssessment aircraft_separation(double distance_m) noexcept {
    return {distance_m < 20.0, distance_m < 12.0};
}

[[nodiscard]] constexpr SeparationAssessment aircraft_ground_separation(double distance_m) noexcept {
    return {distance_m < 30.0, distance_m < 8.0};
}

}  // namespace airside::surface_safety
