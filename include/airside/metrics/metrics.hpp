#pragma once

#include "airside/agents/aircraft.hpp"
#include "airside/agents/service_vehicle.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace airside {

struct AircraftMetrics {
    AircraftId id;
    std::string flight_number;
    SimTime turnaround{};
    SimTime departure_delay{};
    SimTime service_waiting{};

    constexpr auto operator<=>(const AircraftMetrics&) const = default;
};

struct SimulationMetrics {
    std::vector<AircraftMetrics> aircraft;
    double fuel_utilization{0.0};
    double baggage_utilization{0.0};
    std::size_t delayed_aircraft{0};
    double average_turnaround_seconds{0.0};

    constexpr auto operator<=>(const SimulationMetrics&) const = default;
};

[[nodiscard]] SimulationMetrics calculate_metrics(
    const std::vector<Aircraft>& aircraft,
    const std::vector<ServiceVehicle>& vehicles,
    SimTime simulated_duration);

}  // namespace airside
