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
    std::string turnaround_id;
    SimTime target_off_block{};
    std::optional<SimTime> estimated_ready_time;
    std::optional<SimTime> schedule_slack;
    std::optional<SimTime> actual_completion_time;
    std::vector<TaskId> critical_path_tasks;
    struct TaskTiming {
        TaskId task;
        ServiceType service;
        TaskStatus state{TaskStatus::Pending};
        std::optional<SimTime> requested_at;
        std::optional<SimTime> started_at;
        std::optional<SimTime> completed_at;
        SimTime waiting{};
        std::string required_resource;
        std::string assigned_resource;
        auto operator<=>(const TaskTiming&) const = default;
    };
    std::vector<TaskTiming> task_timings;

    constexpr auto operator<=>(const AircraftMetrics&) const = default;
};

struct SimulationMetrics {
    std::vector<AircraftMetrics> aircraft;
    double fuel_utilization{0.0};
    double baggage_utilization{0.0};
    std::size_t delayed_aircraft{0};
    double average_turnaround_seconds{0.0};
    std::size_t total_turnarounds{0};
    std::size_t completed_turnarounds{0};
    std::size_t delayed_turnarounds{0};
    std::size_t failed_or_timed_out_turnarounds{0};
    double mean_departure_delay_seconds{0.0};
    double mean_turnaround_duration_seconds{0.0};
    std::int64_t maximum_turnaround_seconds{0};
    std::int64_t maximum_departure_delay_seconds{0};
    std::size_t on_time_departures{0};
    double on_time_departure_rate{0.0};
    std::int64_t total_service_task_wait_seconds{0};
    std::int64_t maximum_service_task_wait_seconds{0};
    std::vector<std::pair<ServiceType, double>> resource_utilization;
    std::size_t task_reassignments{0};
    std::size_t disruption_triggered_replans{0};
    std::size_t unresolved_service_requests{0};
    std::size_t fleet_collisions{0};
    double fleet_minimum_separation_m{0.0};
    std::size_t fleet_reservation_requests{0};
    std::size_t fleet_reservation_contentions{0};
    std::size_t fleet_outstanding_reservations{0};
    std::size_t fleet_unfinished_requests{0};
    std::size_t fleet_reassignments{0};
    std::size_t fleet_requests_created{0};
    std::size_t fleet_requests_completed{0};
    std::size_t fleet_requests_failed{0};
    std::size_t surface_departed_aircraft{0};
    std::size_t surface_total_aircraft{0};
    double surface_departure_throughput_per_hour{0.0};
    std::size_t surface_reroutes{0};
    std::size_t surface_wait_events{0};
    std::int64_t surface_wait_seconds{0};
    double surface_taxi_distance_m{0.0};
    std::int64_t surface_taxi_seconds{0};
    std::int64_t runway_queue_seconds{0};
    std::size_t surface_safe_failures{0};
    std::size_t max_simultaneous_taxiing_aircraft{0};
    double minimum_aircraft_separation_m{0.0};
    std::size_t surface_aircraft_aircraft_collisions{0};
    std::size_t surface_aircraft_ground_collisions{0};
    double minimum_aircraft_ground_separation_m{0.0};

    constexpr auto operator<=>(const SimulationMetrics&) const = default;
};

[[nodiscard]] SimulationMetrics calculate_metrics(
    const std::vector<Aircraft>& aircraft,
    const std::vector<ServiceVehicle>& vehicles,
    SimTime simulated_duration);

}  // namespace airside
