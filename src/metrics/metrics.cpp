#include "airside/metrics/metrics.hpp"

#include <algorithm>
#include <stdexcept>

namespace airside {

SimulationMetrics calculate_metrics(
    const std::vector<Aircraft>& aircraft,
    const std::vector<ServiceVehicle>& vehicles,
    SimTime simulated_duration) {
    if (simulated_duration <= SimTime::zero()) {
        throw std::invalid_argument("simulated duration must be positive");
    }

    SimulationMetrics result;
    std::int64_t total_turnaround = 0;
    for (const auto& flight : aircraft) {
        if (!flight.actual_arrival() || !flight.actual_departure()) {
            throw std::logic_error("metrics require completed aircraft turnarounds");
        }
        const auto turnaround = *flight.actual_departure() - *flight.actual_arrival();
        const auto departure_delay = std::max(
            SimTime::zero(), *flight.actual_departure() - flight.scheduled_departure());
        SimTime service_waiting{};
        for (const auto& task : flight.tasks()) {
            if (!task.requested_at || !task.started_at) {
                throw std::logic_error("metrics require completed service timing");
            }
            service_waiting += *task.started_at - *task.requested_at;
        }
        result.aircraft.push_back(
            {flight.id(), flight.flight_number(), turnaround, departure_delay, service_waiting});
        total_turnaround += turnaround.count();
        if (departure_delay > SimTime::zero()) {
            ++result.delayed_aircraft;
        }
    }

    if (!aircraft.empty()) {
        result.average_turnaround_seconds =
            static_cast<double>(total_turnaround) / static_cast<double>(aircraft.size());
    }

    auto utilization = [&](ServiceType type) {
        std::int64_t busy_seconds = 0;
        std::size_t count = 0;
        for (const auto& vehicle : vehicles) {
            if (vehicle.capability() == type) {
                busy_seconds += vehicle.busy_time().count();
                ++count;
            }
        }
        if (count == 0) {
            return 0.0;
        }
        const auto capacity = static_cast<double>(simulated_duration.count()) *
            static_cast<double>(count);
        return static_cast<double>(busy_seconds) / capacity;
    };

    result.fuel_utilization = utilization(ServiceType::Fueling);
    result.baggage_utilization = utilization(ServiceType::Baggage);
    return result;
}

}  // namespace airside
