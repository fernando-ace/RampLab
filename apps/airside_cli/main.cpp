#include "airside/operations/simulation.hpp"
#include "airside/scenario/scenario_loader.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

struct Options {
    std::filesystem::path scenario{"scenarios/baseline.yaml"};
    std::optional<std::uint64_t> seed;
    std::optional<std::filesystem::path> event_recording;
    std::optional<std::filesystem::path> metrics_json;
    std::optional<std::filesystem::path> metrics_csv;
    bool verbose{true};
    bool dump_snapshots{false};
};

void print_usage() {
    std::cout
        << "Usage: airside_cli [--scenario FILE] [--seed NUMBER] [--quiet]\n"
        << "                   [--dump-snapshots] [--record-events FILE]\n"
        << "                   [--metrics-json FILE] [--metrics-csv FILE] [--help]\n";
}

Options parse_options(int argc, char* argv[]) {
    Options result;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--help") { print_usage(); std::exit(0); }
        if (argument == "--quiet") { result.verbose = false; continue; }
        if (argument == "--dump-snapshots") { result.dump_snapshots = true; continue; }
        if (argument == "--scenario" || argument == "--record-events" || argument == "--seed" ||
            argument == "--metrics-json" || argument == "--metrics-csv") {
            if (++index >= argc) throw std::invalid_argument(std::format("{} requires a value", argument));
            const std::string value{argv[index]};
            if (argument == "--scenario") result.scenario = value;
            else if (argument == "--record-events") result.event_recording = value;
            else if (argument == "--metrics-json") result.metrics_json = value;
            else if (argument == "--metrics-csv") result.metrics_csv = value;
            else {
                std::size_t consumed = 0;
                result.seed = std::stoull(value, &consumed);
                if (consumed != value.size()) throw std::invalid_argument("--seed requires an unsigned integer");
            }
            continue;
        }
        throw std::invalid_argument(std::format("unknown argument: {}", argument));
    }
    return result;
}

std::string csv_escape(std::string_view value) {
    if (value.find_first_of(",\"\r\n") == std::string_view::npos) return std::string{value};
    std::string output{"\""};
    for (const auto character : value) {
        if (character == '"') output += "\"\"";
        else output += character;
    }
    output += '"';
    return output;
}

std::string json_escape(std::string_view text);

void write_metrics(const std::filesystem::path& json_path, const std::filesystem::path& csv_path,
                   const airside::SimulationResult& result, const airside::SimulationSnapshot& snapshot) {
    if (!json_path.empty()) {
        if (!json_path.parent_path().empty()) std::filesystem::create_directories(json_path.parent_path());
        std::ofstream output{json_path};
        if (!output) throw std::runtime_error("cannot create metrics JSON: " + json_path.string());
        const auto& metrics = result.metrics;
        output << std::fixed << std::setprecision(6)
            << "{\"seed\":" << result.seed << ",\"simulated_duration_seconds\":" << result.simulated_duration.count()
            << ",\"total_turnarounds\":" << metrics.total_turnarounds
            << ",\"completed_turnarounds\":" << metrics.completed_turnarounds
            << ",\"delayed_turnarounds\":" << metrics.delayed_turnarounds
            << ",\"failed_or_timed_out_turnarounds\":" << metrics.failed_or_timed_out_turnarounds
            << ",\"task_reassignments\":" << metrics.task_reassignments
            << ",\"disruption_triggered_replans\":" << metrics.disruption_triggered_replans
            << ",\"unresolved_service_requests\":" << metrics.unresolved_service_requests
            << ",\"fleet\":{\"collisions\":" << metrics.fleet_collisions
            << ",\"minimum_separation_m\":" << metrics.fleet_minimum_separation_m
            << ",\"reservation_requests\":" << metrics.fleet_reservation_requests
            << ",\"reservation_contentions\":" << metrics.fleet_reservation_contentions
            << ",\"outstanding_reservations\":" << metrics.fleet_outstanding_reservations
            << ",\"unfinished_requests\":" << metrics.fleet_unfinished_requests
            << ",\"reassignments\":" << metrics.fleet_reassignments
            << ",\"requests_created\":" << metrics.fleet_requests_created
            << ",\"requests_completed\":" << metrics.fleet_requests_completed
            << ",\"requests_failed\":" << metrics.fleet_requests_failed << "},\"surface\":{\"departed_aircraft\":"
            << metrics.surface_departed_aircraft << ",\"total_aircraft\":" << metrics.surface_total_aircraft
            << ",\"departure_throughput_per_hour\":" << metrics.surface_departure_throughput_per_hour
            << ",\"reroutes\":" << metrics.surface_reroutes << ",\"wait_events\":" << metrics.surface_wait_events
            << ",\"wait_seconds\":" << metrics.surface_wait_seconds << ",\"taxi_distance_m\":" << metrics.surface_taxi_distance_m
            << ",\"taxi_seconds\":" << metrics.surface_taxi_seconds << ",\"runway_queue_seconds\":" << metrics.runway_queue_seconds
            << ",\"safe_failures\":" << metrics.surface_safe_failures << ",\"max_simultaneous_taxiing\":"
            << metrics.max_simultaneous_taxiing_aircraft << ",\"minimum_aircraft_separation_m\":"
            << metrics.minimum_aircraft_separation_m << ",\"minimum_aircraft_ground_separation_m\":"
            << metrics.minimum_aircraft_ground_separation_m << ",\"aircraft_aircraft_collisions\":"
            << metrics.surface_aircraft_aircraft_collisions << ",\"aircraft_ground_collisions\":"
            << metrics.surface_aircraft_ground_collisions
            << ",\"arrivals_completed\":" << metrics.surface_arrived_aircraft
            << ",\"gate_assignments\":" << metrics.gate_assignments
            << ",\"gate_wait_seconds\":" << metrics.gate_wait_seconds
            << ",\"gate_occupancy_seconds\":" << metrics.gate_occupancy_seconds
            << ",\"arrival_to_departure_seconds\":" << metrics.arrival_to_departure_seconds
            << ",\"runway_operations_completed\":" << metrics.runway_operations_completed
            << ",\"maximum_runway_queue_depth\":" << metrics.maximum_runway_queue_depth
            << ",\"arrival_runway_wait_seconds\":" << metrics.arrival_runway_wait_seconds
            << ",\"departure_runway_wait_seconds\":" << metrics.departure_runway_wait_seconds
            << ",\"average_runway_wait_seconds\":" << metrics.average_runway_wait_seconds
            << ",\"runway_occupied_seconds\":" << metrics.runway_occupied_seconds
            << ",\"runway_utilization\":" << metrics.runway_utilization
            << ",\"arrival_taxi_seconds\":" << metrics.arrival_taxi_seconds
            << ",\"arrival_taxi_distance_m\":" << metrics.arrival_taxi_distance_m
            << ",\"departure_taxi_seconds\":" << metrics.departure_taxi_seconds
            << ",\"departure_taxi_distance_m\":" << metrics.departure_taxi_distance_m
            << "},\"aircraft_operations\":[";
        for (std::size_t index = 0; index < snapshot.aircraft.size(); ++index) {
            const auto& item = snapshot.aircraft[index];
            if (index) output << ',';
            output << "{\"aircraft_id\":" << item.id.value() << ",\"flight_number\":\"" << json_escape(item.flight_number)
                << "\",\"operation_type\":\"" << item.operation_type << "\",\"surface_state\":\"" << item.surface_state
                << "\",\"gate_id\":" << item.assigned_gate.value()
                << ",\"actual_arrival_seconds\":" << (item.actual_arrival ? std::to_string(item.actual_arrival->count()) : "null")
                << ",\"runway_request_time_seconds\":" << (item.runway_queue_entered_at ? std::to_string(item.runway_queue_entered_at->count()) : "null")
                << ",\"runway_clearance_time_seconds\":" << (item.runway_clearance_at ? std::to_string(item.runway_clearance_at->count()) : "null")
                << ",\"runway_wait_seconds\":" << item.runway_wait_duration.count()
                << ",\"arrival_runway_request_time_seconds\":" << (item.arrival_runway_request_time ? std::to_string(item.arrival_runway_request_time->count()) : "null")
                << ",\"arrival_runway_clearance_time_seconds\":" << (item.arrival_runway_clearance_time ? std::to_string(item.arrival_runway_clearance_time->count()) : "null")
                << ",\"arrival_runway_wait_seconds\":" << item.arrival_runway_wait_duration.count()
                << ",\"arrival_runway_release_time_seconds\":" << (item.arrival_runway_release_time ? std::to_string(item.arrival_runway_release_time->count()) : "null")
                << ",\"departure_runway_request_time_seconds\":" << (item.departure_runway_request_time ? std::to_string(item.departure_runway_request_time->count()) : "null")
                << ",\"departure_runway_clearance_time_seconds\":" << (item.departure_runway_clearance_time ? std::to_string(item.departure_runway_clearance_time->count()) : "null")
                << ",\"departure_runway_wait_seconds\":" << item.departure_runway_wait_duration.count()
                << ",\"departure_runway_release_time_seconds\":" << (item.departure_runway_release_time ? std::to_string(item.departure_runway_release_time->count()) : "null")
                << ",\"runway_release_time_seconds\":" << (item.runway_release_at ? std::to_string(item.runway_release_at->count()) : "null")
                << ",\"arrival_gate_time_seconds\":" << (item.arrival_gate_at ? std::to_string(item.arrival_gate_at->count()) : "null")
                << ",\"gate_wait_seconds\":" << item.gate_wait_duration.count()
                << ",\"gate_occupancy_seconds\":" << item.gate_occupancy_duration.count()
                << ",\"gate_released_at_seconds\":" << (item.gate_released_at ? std::to_string(item.gate_released_at->count()) : "null")
                << ",\"pushback_started_at_seconds\":" << (item.pushback_started_at ? std::to_string(item.pushback_started_at->count()) : "null")
                << ",\"pushback_completed_at_seconds\":" << (item.pushback_completed_at ? std::to_string(item.pushback_completed_at->count()) : "null")
                << ",\"arrival_taxi_started_at_seconds\":" << (item.arrival_taxi_started_at ? std::to_string(item.arrival_taxi_started_at->count()) : "null")
                << ",\"arrival_taxi_completed_at_seconds\":" << (item.arrival_taxi_completed_at ? std::to_string(item.arrival_taxi_completed_at->count()) : "null")
                << ",\"arrival_taxi_distance_m\":" << item.arrival_taxi_distance_m
                << ",\"departure_taxi_started_at_seconds\":" << (item.departure_taxi_started_at ? std::to_string(item.departure_taxi_started_at->count()) : "null")
                << ",\"departure_taxi_completed_at_seconds\":" << (item.departure_taxi_completed_at ? std::to_string(item.departure_taxi_completed_at->count()) : "null")
                << ",\"departure_taxi_distance_m\":" << item.departure_taxi_distance_m
                << ",\"arrival_to_departure_seconds\":" << (item.actual_surface_departure && item.actual_arrival ? std::to_string((*item.actual_surface_departure - *item.actual_arrival).count()) : "null")
                << ",\"taxi_distance_m\":" << item.taxi_distance_m
                << ",\"departure_time_seconds\":" << (item.actual_surface_departure ? std::to_string(item.actual_surface_departure->count()) : "null") << "}";
        }
        output << "],\"turnarounds\":[";
        for (std::size_t index = 0; index < snapshot.turnarounds.size(); ++index) {
            const auto& turnaround = snapshot.turnarounds[index];
            const auto aircraft = std::ranges::find(snapshot.aircraft, turnaround.aircraft,
                &airside::AircraftSnapshot::id);
            const auto completed_tasks = std::ranges::count_if(turnaround.tasks, [](const auto& task) {
                return task.status == airside::TaskStatus::Completed;
            });
            const auto unfinished_tasks = turnaround.tasks.size() - completed_tasks;
            if (index != 0) output << ',';
            output << "{\"turnaround_id\":\"" << json_escape(turnaround.turnaround_id)
                << "\",\"aircraft_id\":" << turnaround.aircraft.value()
                << ",\"gate_id\":" << turnaround.gate.value()
                << ",\"state\":\"" << airside::to_string(turnaround.state)
                << "\",\"failure_reason\":\"" << json_escape(turnaround.failure_reason)
                << "\",\"scheduled_arrival_seconds\":" << turnaround.scheduled_arrival.count()
                << ",\"actual_arrival_seconds\":" << (turnaround.actual_arrival ? std::to_string(turnaround.actual_arrival->count()) : "null")
                << ",\"scheduled_departure_seconds\":" << turnaround.scheduled_departure.count()
                << ",\"actual_departure_seconds\":" << (aircraft != snapshot.aircraft.end() && aircraft->actual_departure ? std::to_string(aircraft->actual_departure->count()) : "null")
                << ",\"departure_delay_seconds\":" << (turnaround.departure_delay ? std::to_string(turnaround.departure_delay->count()) : "null")
                << ",\"surface_state\":\"" << (aircraft != snapshot.aircraft.end() ? json_escape(aircraft->surface_state) : "") << "\""
                << ",\"surface_reroutes\":" << (aircraft != snapshot.aircraft.end() ? aircraft->surface_reroutes : 0)
                << ",\"taxi_distance_m\":" << (aircraft != snapshot.aircraft.end() ? aircraft->taxi_distance_m : 0.0)
                << ",\"surface_wait_seconds\":" << (aircraft != snapshot.aircraft.end() ? aircraft->surface_wait_duration.count() : 0)
                << ",\"gate_wait_seconds\":" << (aircraft != snapshot.aircraft.end() ? aircraft->gate_wait_duration.count() : 0)
                << ",\"gate_occupancy_seconds\":" << (aircraft != snapshot.aircraft.end() ? aircraft->gate_occupancy_duration.count() : 0)
                << ",\"pushback_started_at_seconds\":" << (aircraft != snapshot.aircraft.end() && aircraft->pushback_started_at ? std::to_string(aircraft->pushback_started_at->count()) : "null")
                << ",\"pushback_completed_at_seconds\":" << (aircraft != snapshot.aircraft.end() && aircraft->pushback_completed_at ? std::to_string(aircraft->pushback_completed_at->count()) : "null")
                << ",\"taxi_started_at_seconds\":" << (aircraft != snapshot.aircraft.end() && aircraft->taxi_started_at ? std::to_string(aircraft->taxi_started_at->count()) : "null")
                << ",\"taxi_completed_at_seconds\":" << (aircraft != snapshot.aircraft.end() && aircraft->taxi_completed_at ? std::to_string(aircraft->taxi_completed_at->count()) : "null")
                << ",\"runway_queue_entered_at_seconds\":" << (aircraft != snapshot.aircraft.end() && aircraft->runway_queue_entered_at ? std::to_string(aircraft->runway_queue_entered_at->count()) : "null")
                << ",\"completed_required_tasks\":" << completed_tasks
                << ",\"unfinished_required_tasks\":" << unfinished_tasks
                << ",\"estimated_ready_time_seconds\":" << turnaround.estimated_ready_time.count()
                << ",\"schedule_slack_seconds\":" << turnaround.schedule_slack.count()
                << ",\"predicted_late\":" << (turnaround.predicted_late ? "true" : "false")
                << ",\"critical_path_task_ids\":[";
            for (std::size_t path_index = 0; path_index < turnaround.critical_path_tasks.size(); ++path_index) {
                if (path_index != 0) output << ',';
                output << turnaround.critical_path_tasks[path_index].value();
            }
            output << "],\"tasks\":[";
            for (std::size_t task_index = 0; task_index < turnaround.tasks.size(); ++task_index) {
                const auto& task = turnaround.tasks[task_index];
                if (task_index != 0) output << ',';
                output << "{\"task_id\":" << task.id.value() << ",\"service_type\":\""
                    << airside::to_string(task.type) << "\",\"state\":\"" << airside::to_string(task.status)
                    << "\",\"requested_at_seconds\":" << (task.requested_at ? std::to_string(task.requested_at->count()) : "null")
                    << ",\"started_at_seconds\":" << (task.started_at ? std::to_string(task.started_at->count()) : "null")
                    << ",\"completed_at_seconds\":" << (task.completed_at ? std::to_string(task.completed_at->count()) : "null")
                    << ",\"latest_desirable_completion_seconds\":" << (task.latest_desirable_completion ? std::to_string(task.latest_desirable_completion->count()) : "null")
                    << ",\"reassignments\":" << task.reassignments << ",\"assigned_resource\":\""
                    << json_escape(task.assigned_resource) << "\"}";
            }
            output << "]}";
        }
        output << "]}\n";
        if (!output) throw std::runtime_error("failed writing metrics JSON: " + json_path.string());
    }
    if (!csv_path.empty()) {
        if (!csv_path.parent_path().empty()) std::filesystem::create_directories(csv_path.parent_path());
        std::ofstream output{csv_path};
        if (!output) throw std::runtime_error("cannot create metrics CSV: " + csv_path.string());
        output << "seed,simulated_duration_seconds,total_turnarounds,completed_turnarounds,delayed_turnarounds,failed_or_timed_out_turnarounds,task_reassignments,disruption_triggered_replans,unresolved_service_requests,fleet_collisions,fleet_minimum_separation_m,fleet_reservation_requests,fleet_reservation_contentions,fleet_outstanding_reservations,fleet_unfinished_requests,fleet_reassignments,fleet_requests_created,fleet_requests_completed,fleet_requests_failed,turnaround_id,aircraft_id,gate_id,scheduled_departure_seconds,actual_departure_seconds,departure_delay_seconds,completed_required_tasks,unfinished_required_tasks,task_id,service_type,state,requested_at_seconds,started_at_seconds,completed_at_seconds,latest_desirable_completion_seconds,reassignments,assigned_resource,surface_state,taxi_distance_m,surface_reroutes,surface_wait_seconds,pushback_started_at_seconds,pushback_completed_at_seconds,taxi_started_at_seconds,taxi_completed_at_seconds,runway_queue_entered_at_seconds,surface_departed_aircraft,surface_total_aircraft,surface_departure_throughput_per_hour,surface_reroutes_total,surface_wait_events,surface_wait_total_seconds,surface_taxi_seconds,runway_queue_total_seconds,surface_safe_failures,max_simultaneous_taxiing_aircraft,surface_aircraft_aircraft_collisions,surface_aircraft_ground_collisions,minimum_aircraft_separation_m,minimum_aircraft_ground_separation_m,arrivals_completed,gate_assignments,gate_wait_seconds,gate_occupancy_seconds,arrival_to_departure_seconds,runway_operations_completed,maximum_runway_queue_depth,arrival_runway_wait_seconds,departure_runway_wait_seconds,average_runway_wait_seconds,runway_occupied_seconds,runway_utilization,arrival_taxi_seconds,arrival_taxi_distance_m,departure_taxi_seconds,departure_taxi_distance_m\n";
        output << result.seed << ',' << result.simulated_duration.count() << ',' << result.metrics.total_turnarounds << ','
            << result.metrics.completed_turnarounds << ',' << result.metrics.delayed_turnarounds << ','
            << result.metrics.failed_or_timed_out_turnarounds << ',' << result.metrics.task_reassignments << ','
            << result.metrics.disruption_triggered_replans << ',' << result.metrics.unresolved_service_requests << ','
            << result.metrics.fleet_collisions << ',' << result.metrics.fleet_minimum_separation_m << ','
            << result.metrics.fleet_reservation_requests << ',' << result.metrics.fleet_reservation_contentions << ','
            << result.metrics.fleet_outstanding_reservations << ',' << result.metrics.fleet_unfinished_requests << ','
            << result.metrics.fleet_reassignments << ',' << result.metrics.fleet_requests_created << ','
            << result.metrics.fleet_requests_completed << ',' << result.metrics.fleet_requests_failed << ',';
        bool first = true;
        for (const auto& turnaround : snapshot.turnarounds) {
            for (const auto& task : turnaround.tasks) {
                if (!first) output << "\n" << result.seed << ',' << result.simulated_duration.count() << ','
                    << result.metrics.total_turnarounds << ',' << result.metrics.completed_turnarounds << ','
                    << result.metrics.delayed_turnarounds << ',' << result.metrics.failed_or_timed_out_turnarounds << ','
                    << result.metrics.task_reassignments << ',' << result.metrics.disruption_triggered_replans << ','
                    << result.metrics.unresolved_service_requests << ',' << result.metrics.fleet_collisions << ','
                    << result.metrics.fleet_minimum_separation_m << ',' << result.metrics.fleet_reservation_requests << ','
                    << result.metrics.fleet_reservation_contentions << ',' << result.metrics.fleet_outstanding_reservations << ','
                    << result.metrics.fleet_unfinished_requests << ',' << result.metrics.fleet_reassignments << ','
                    << result.metrics.fleet_requests_created << ',' << result.metrics.fleet_requests_completed << ','
                    << result.metrics.fleet_requests_failed << ',';
                first = false;
                const auto aircraft = std::ranges::find(snapshot.aircraft, turnaround.aircraft,
                    &airside::AircraftSnapshot::id);
                const auto completed_tasks = std::ranges::count_if(turnaround.tasks, [](const auto& item) {
                    return item.status == airside::TaskStatus::Completed;
                });
                output << csv_escape(turnaround.turnaround_id) << ',' << turnaround.aircraft.value() << ','
                    << turnaround.gate.value() << ',' << turnaround.scheduled_departure.count() << ','
                    << (aircraft != snapshot.aircraft.end() && aircraft->actual_departure ? std::to_string(aircraft->actual_departure->count()) : "") << ','
                    << (turnaround.departure_delay ? std::to_string(turnaround.departure_delay->count()) : "") << ','
                    << completed_tasks << ',' << (turnaround.tasks.size() - completed_tasks) << ',' << task.id.value() << ','
                    << airside::to_string(task.type) << ',' << airside::to_string(task.status) << ','
                    << (task.requested_at ? std::to_string(task.requested_at->count()) : "") << ','
                    << (task.started_at ? std::to_string(task.started_at->count()) : "") << ','
                    << (task.completed_at ? std::to_string(task.completed_at->count()) : "") << ','
                    << (task.latest_desirable_completion ? std::to_string(task.latest_desirable_completion->count()) : "") << ','
                    << task.reassignments << ',' << csv_escape(task.assigned_resource) << ','
                    << (aircraft != snapshot.aircraft.end() ? aircraft->surface_state : "") << ','
                    << (aircraft != snapshot.aircraft.end() ? aircraft->taxi_distance_m : 0.0) << ','
                    << (aircraft != snapshot.aircraft.end() ? aircraft->surface_reroutes : 0) << ','
                    << (aircraft != snapshot.aircraft.end() ? aircraft->surface_wait_duration.count() : 0) << ','
                    << (aircraft != snapshot.aircraft.end() && aircraft->pushback_started_at ? std::to_string(aircraft->pushback_started_at->count()) : "") << ','
                    << (aircraft != snapshot.aircraft.end() && aircraft->pushback_completed_at ? std::to_string(aircraft->pushback_completed_at->count()) : "") << ','
                    << (aircraft != snapshot.aircraft.end() && aircraft->taxi_started_at ? std::to_string(aircraft->taxi_started_at->count()) : "") << ','
                    << (aircraft != snapshot.aircraft.end() && aircraft->taxi_completed_at ? std::to_string(aircraft->taxi_completed_at->count()) : "") << ','
                    << (aircraft != snapshot.aircraft.end() && aircraft->runway_queue_entered_at ? std::to_string(aircraft->runway_queue_entered_at->count()) : "") << ','
                    << result.metrics.surface_departed_aircraft << ',' << result.metrics.surface_total_aircraft << ','
                    << result.metrics.surface_departure_throughput_per_hour << ','
                    << result.metrics.surface_reroutes << ',' << result.metrics.surface_wait_events << ','
                    << result.metrics.surface_wait_seconds << ',' << result.metrics.surface_taxi_seconds << ','
                    << result.metrics.runway_queue_seconds << ',' << result.metrics.surface_safe_failures << ','
                    << result.metrics.max_simultaneous_taxiing_aircraft << ','
                    << result.metrics.surface_aircraft_aircraft_collisions << ','
                    << result.metrics.surface_aircraft_ground_collisions << ','
                    << result.metrics.minimum_aircraft_separation_m << ','
                    << result.metrics.minimum_aircraft_ground_separation_m << ','
                    << result.metrics.surface_arrived_aircraft << ',' << result.metrics.gate_assignments << ','
                    << result.metrics.gate_wait_seconds << ',' << result.metrics.gate_occupancy_seconds << ','
                    << result.metrics.arrival_to_departure_seconds << ',' << result.metrics.runway_operations_completed << ','
                    << result.metrics.maximum_runway_queue_depth << ',' << result.metrics.arrival_runway_wait_seconds << ','
                    << result.metrics.departure_runway_wait_seconds << ',' << result.metrics.average_runway_wait_seconds << ','
                    << result.metrics.runway_occupied_seconds << ',' << result.metrics.runway_utilization << ','
                    << result.metrics.arrival_taxi_seconds << ',' << result.metrics.arrival_taxi_distance_m << ','
                    << result.metrics.departure_taxi_seconds << ',' << result.metrics.departure_taxi_distance_m;
            }
        }
        output << '\n';
        if (!output) throw std::runtime_error("failed writing metrics CSV: " + csv_path.string());

        auto aircraft_csv_path = csv_path;
        aircraft_csv_path.replace_filename(csv_path.stem().string() + ".aircraft.csv");
        std::ofstream aircraft_csv{aircraft_csv_path};
        if (!aircraft_csv) throw std::runtime_error("cannot create aircraft operations CSV: " + aircraft_csv_path.string());
        aircraft_csv << "aircraft_id,flight_number,operation_type,surface_state,gate_id,scheduled_arrival_seconds,actual_arrival_seconds,runway_request_time_seconds,runway_clearance_time_seconds,runway_wait_seconds,runway_release_time_seconds,runway_occupancy_seconds,arrival_taxi_started_at_seconds,arrival_taxi_completed_at_seconds,arrival_taxi_seconds,arrival_taxi_distance_m,gate_assigned_at_seconds,gate_wait_seconds,gate_arrival_time_seconds,gate_occupancy_seconds,gate_released_at_seconds,pushback_started_at_seconds,pushback_completed_at_seconds,departure_taxi_started_at_seconds,departure_taxi_completed_at_seconds,departure_taxi_seconds,departure_taxi_distance_m,arrival_to_departure_seconds,departure_time_seconds,total_operational_delay_seconds\n";
        for (const auto& item : snapshot.aircraft) {
            const auto arrival_taxi_seconds = item.arrival_taxi_started_at && item.arrival_taxi_completed_at
                ? (*item.arrival_taxi_completed_at - *item.arrival_taxi_started_at).count() : 0;
            const auto departure_taxi_seconds = item.departure_taxi_started_at && item.departure_taxi_completed_at
                ? (*item.departure_taxi_completed_at - *item.departure_taxi_started_at).count() : 0;
            const auto runway_occupancy = item.runway_clearance_at && item.runway_release_at
                ? (*item.runway_release_at - *item.runway_clearance_at).count() : 0;
            const auto operation_metrics = std::ranges::find(result.metrics.aircraft, item.id, &airside::AircraftMetrics::id);
            aircraft_csv << item.id.value() << ',' << csv_escape(item.flight_number) << ',' << item.operation_type << ','
                << item.surface_state << ',' << item.assigned_gate.value() << ',' << item.scheduled_arrival.count() << ','
                << (item.actual_arrival ? std::to_string(item.actual_arrival->count()) : "") << ','
                << (item.runway_queue_entered_at ? std::to_string(item.runway_queue_entered_at->count()) : "") << ','
                << (item.runway_clearance_at ? std::to_string(item.runway_clearance_at->count()) : "") << ','
                << item.runway_wait_duration.count() << ','
                << (item.runway_release_at ? std::to_string(item.runway_release_at->count()) : "") << ','
                << runway_occupancy << ','
                << (item.arrival_taxi_started_at ? std::to_string(item.arrival_taxi_started_at->count()) : "") << ','
                << (item.arrival_taxi_completed_at ? std::to_string(item.arrival_taxi_completed_at->count()) : "") << ','
                << arrival_taxi_seconds << ',' << item.arrival_taxi_distance_m << ','
                << (item.gate_assigned_at ? std::to_string(item.gate_assigned_at->count()) : "") << ','
                << item.gate_wait_duration.count() << ','
                << (item.arrival_gate_at ? std::to_string(item.arrival_gate_at->count()) : "") << ','
                << item.gate_occupancy_duration.count() << ','
                << (item.gate_released_at ? std::to_string(item.gate_released_at->count()) : "") << ','
                << (item.pushback_started_at ? std::to_string(item.pushback_started_at->count()) : "") << ','
                << (item.pushback_completed_at ? std::to_string(item.pushback_completed_at->count()) : "") << ','
                << (item.departure_taxi_started_at ? std::to_string(item.departure_taxi_started_at->count()) : "") << ','
                << (item.departure_taxi_completed_at ? std::to_string(item.departure_taxi_completed_at->count()) : "") << ','
                << departure_taxi_seconds << ',' << item.departure_taxi_distance_m << ','
                << (item.actual_surface_departure && item.actual_arrival ? std::to_string((*item.actual_surface_departure - *item.actual_arrival).count()) : "") << ','
                << (item.actual_surface_departure ? std::to_string(item.actual_surface_departure->count()) : "") << ','
                << (operation_metrics != result.metrics.aircraft.end() ? operation_metrics->total_operational_delay.count() : 0) << '\n';
        }
        if (!aircraft_csv) throw std::runtime_error("failed writing aircraft operations CSV: " + aircraft_csv_path.string());
    }
}

class ConsoleEventSink final : public airside::ISimulationEventSink {
public:
    void on_event(const airside::SimulationEventRecord& event) noexcept override {
        try {
            std::cout << airside::format_sim_time(event.timestamp) << "  "
                      << airside::format_event(event) << '\n';
        } catch (...) { failed_ = true; }
    }
    [[nodiscard]] bool failed() const noexcept { return failed_; }
private:
    bool failed_{false};
};

std::string json_escape(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (const char character : text) {
        switch (character) {
        case '\\': result += "\\\\"; break;
        case '"': result += "\\\""; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default: result += character; break;
        }
    }
    return result;
}

class JsonLinesEventSink final : public airside::ISimulationEventSink {
public:
    explicit JsonLinesEventSink(const std::filesystem::path& path) : output_(path) {
        if (!output_) throw std::runtime_error(std::format("cannot open event recording '{}'", path.string()));
    }

    void on_event(const airside::SimulationEventRecord& event) noexcept override {
        try {
            output_ << "{\"sequence\":" << event.sequence
                    << ",\"time_seconds\":" << event.timestamp.count()
                    << ",\"type\":\"" << airside::to_string(event.type) << '"';
            if (event.aircraft) output_ << ",\"aircraft_id\":" << event.aircraft->value();
            if (!event.aircraft_name.empty()) output_ << ",\"aircraft\":\"" << json_escape(event.aircraft_name) << '"';
            if (event.vehicle) output_ << ",\"vehicle_id\":" << event.vehicle->value();
            if (!event.vehicle_name.empty()) output_ << ",\"vehicle\":\"" << json_escape(event.vehicle_name) << '"';
            if (!event.turnaround_id.empty()) output_ << ",\"turnaround_id\":\"" << json_escape(event.turnaround_id) << '"';
            if (event.task) output_ << ",\"task_id\":" << event.task->value();
            if (event.edge) output_ << ",\"edge_id\":" << event.edge->value();
            if (event.service) output_ << ",\"service\":\"" << airside::to_string(*event.service) << '"';
            if (event.estimated_ready_time) output_ << ",\"estimated_ready_time_seconds\":" << event.estimated_ready_time->count();
            if (event.schedule_slack) output_ << ",\"schedule_slack_seconds\":" << event.schedule_slack->count();
            if (event.task_duration) output_ << ",\"task_duration_seconds\":" << event.task_duration->count();
            if (!event.detail.empty()) output_ << ",\"detail\":\"" << json_escape(event.detail) << '"';
            if (event.route) {
                output_ << ",\"route_nodes\":[";
                for (std::size_t index = 0; index < event.route->nodes.size(); ++index) {
                    if (index != 0) output_ << ',';
                    output_ << event.route->nodes[index].value();
                }
                output_ << ']';
            }
            output_ << "}\n";
            if (!output_) failed_ = true;
        } catch (...) { failed_ = true; }
    }
    [[nodiscard]] bool failed() const noexcept { return failed_; }
private:
    std::ofstream output_;
    bool failed_{false};
};

void print_snapshot(const airside::SimulationSnapshot& snapshot) {
    std::size_t occupied = 0;
    std::size_t traveling = 0;
    std::size_t closed = 0;
    for (const auto& gate : snapshot.gates) occupied += gate.occupying_aircraft.has_value() ? 1U : 0U;
    for (const auto& vehicle : snapshot.vehicles) traveling += vehicle.journey.has_value() ? 1U : 0U;
    for (const auto& road : snapshot.roads) closed += road.enabled ? 0U : 1U;
    std::cout << "SNAPSHOT v" << snapshot.version.value
              << " time=" << airside::format_sim_time(snapshot.simulation_time)
              << " occupied_gates=" << occupied
              << " traveling_vehicles=" << traveling
              << " closed_roads=" << closed << '\n';
    for (const auto& vehicle : snapshot.vehicles) {
        std::cout << "  ground_vehicle=" << vehicle.name
                  << " status=" << vehicle.fleet_status
                  << " pose=" << (vehicle.observed_position_m
                      ? std::format("{:.1f},{:.1f}", vehicle.observed_position_m->x_m, vehicle.observed_position_m->y_m)
                      : "unknown") << '\n';
        if (vehicle.journey) {
            std::cout << "  vehicle=" << vehicle.name
                      << " origin=" << vehicle.journey->origin.value()
                      << " destination=" << vehicle.journey->destination.value()
                      << " depart=" << vehicle.journey->departure_time.count()
                      << " arrive=" << vehicle.journey->expected_arrival_time.count()
                      << " segments=" << vehicle.journey->segments.size() << '\n';
        }
    }
    for (const auto& aircraft : snapshot.aircraft) {
        std::cout << "  aircraft=" << aircraft.flight_number
                  << " lifecycle=" << airside::to_string(aircraft.state)
                  << " surface=" << aircraft.surface_state
                  << " gate=" << aircraft.assigned_gate.value()
                  << " node=" << (aircraft.surface_node ? std::to_string(aircraft.surface_node->value()) : "none")
                  << " wait=" << aircraft.surface_wait_duration.count()
                  << " runway_wait=" << aircraft.runway_wait_duration.count()
                  << " departed=" << (aircraft.actual_departure ? "true" : "false") << '\n';
    }
    for (const auto& turnaround : snapshot.turnarounds) {
        std::size_t completed = 0;
        std::size_t active = 0;
        for (const auto& task : turnaround.tasks) {
            completed += task.status == airside::TaskStatus::Completed ? 1U : 0U;
            active += task.status == airside::TaskStatus::InProgress || task.status == airside::TaskStatus::Assigned ? 1U : 0U;
        }
        std::cout << "  turnaround=" << turnaround.turnaround_id
                  << " state=" << airside::to_string(turnaround.state)
                  << " tasks=" << completed << '/' << turnaround.tasks.size()
                  << " active=" << active
                  << " estimated_ready=" << turnaround.estimated_ready_time.count()
                  << " slack=" << turnaround.schedule_slack.count() << "s critical_path=";
        for (std::size_t index = 0; index < turnaround.critical_path_tasks.size(); ++index) {
            if (index != 0) std::cout << ',';
            std::cout << turnaround.critical_path_tasks[index].value();
        }
        std::cout << '\n';
    }
}

double minutes(airside::SimTime value) { return static_cast<double>(value.count()) / 60.0; }

std::uint64_t turnaround_event_digest(const std::vector<airside::SimulationEventRecord>& events) {
    std::uint64_t hash = 14695981039346656037ULL;
    const auto append = [&](std::string_view value) {
        for (const auto byte : value) { hash ^= static_cast<unsigned char>(byte); hash *= 1099511628211ULL; }
    };
    for (const auto& event : events) {
        append(airside::to_string(event.type));
        append(std::to_string(event.sequence));
        append(std::to_string(event.timestamp.count()));
        append(event.turnaround_id);
        if (event.task) append(std::to_string(event.task->value()));
        if (event.aircraft) append(std::to_string(event.aircraft->value()));
        if (event.vehicle) append(std::to_string(event.vehicle->value()));
        if (event.service) append(airside::to_string(*event.service));
    }
    return hash;
}

void print_report(
    const airside::SimulationResult& result,
    std::string_view scenario_name,
    double execution_seconds) {
    std::cout << "\n=== Simulation Complete ===\n\n"
              << "Scenario: " << scenario_name << '\n'
              << "Seed: " << result.seed << '\n'
              << "Simulated duration: " << airside::format_sim_time(result.simulated_duration) << "\n\n"
              << "Aircraft\n";
    for (const auto& aircraft : result.metrics.aircraft) {
        std::cout << aircraft.flight_number << '\n'
                  << "  Turnaround: " << std::fixed << std::setprecision(1) << minutes(aircraft.turnaround) << " min\n"
                  << "  Departure delay: " << minutes(aircraft.departure_delay) << " min\n"
                  << "  Service waiting: " << minutes(aircraft.service_waiting) << " min\n";
    }
    std::cout << "\nFleet\n"
              << "Fuel truck utilization: " << result.metrics.fuel_utilization * 100.0 << "%\n"
              << "Baggage cart utilization: " << result.metrics.baggage_utilization * 100.0 << "%\n\n"
              << "Summary\nAverage turnaround: "
              << result.metrics.average_turnaround_seconds / 60.0 << " min\n"
              << "Delayed aircraft: " << result.metrics.delayed_aircraft << " / "
              << result.metrics.aircraft.size() << "\n";
    if (result.metrics.total_turnarounds != 0) {
        std::cout << "\nTurnaround Operations\n"
                  << "Completed: " << result.metrics.completed_turnarounds << " / " << result.metrics.total_turnarounds
                  << "  Delayed: " << result.metrics.delayed_turnarounds
                  << "  Failed/timeouts: " << result.metrics.failed_or_timed_out_turnarounds << '\n'
                  << "Mean departure delay: " << result.metrics.mean_departure_delay_seconds / 60.0
                  << " min  On-time rate: " << result.metrics.on_time_departure_rate * 100.0 << "%\n"
                  << "Task wait total/max: " << result.metrics.total_service_task_wait_seconds << "/"
                  << result.metrics.maximum_service_task_wait_seconds << " sec  Replans: "
                  << result.metrics.disruption_triggered_replans << "  Reassignments: "
                  << result.metrics.task_reassignments << "  Unresolved service requests: "
                  << result.metrics.unresolved_service_requests << '\n'
                  << "Fleet safety: collisions=" << result.metrics.fleet_collisions
                  << " minimum_separation_m=" << result.metrics.fleet_minimum_separation_m
                  << " reservations=" << result.metrics.fleet_reservation_requests
                  << " contentions=" << result.metrics.fleet_reservation_contentions
                  << " outstanding_reservations=" << result.metrics.fleet_outstanding_reservations
                  << " service_requests=" << result.metrics.fleet_requests_created << '/'
                  << result.metrics.fleet_requests_completed << '/' << result.metrics.fleet_requests_failed
                  << " unfinished_requests=" << result.metrics.fleet_unfinished_requests << '\n'
                  << "Deterministic event digest: " << turnaround_event_digest(result.events) << '\n';
        for (const auto& [type, utilization] : result.metrics.resource_utilization) {
            std::cout << "Resource utilization " << airside::to_string(type) << ": "
                      << utilization * 100.0 << "%\n";
        }
        for (const auto& aircraft : result.metrics.aircraft) {
            std::cout << aircraft.turnaround_id << " / " << aircraft.flight_number
                      << " ready=" << (aircraft.estimated_ready_time ? aircraft.estimated_ready_time->count() : 0)
                      << " sec slack=" << (aircraft.schedule_slack ? aircraft.schedule_slack->count() : 0)
                      << " sec critical_path=";
            for (std::size_t index = 0; index < aircraft.critical_path_tasks.size(); ++index) {
                if (index != 0) std::cout << ',';
                std::cout << aircraft.critical_path_tasks[index].value();
            }
            std::cout << '\n';
            for (const auto& task : aircraft.task_timings) {
                std::cout << "  task=" << task.task.value() << ' ' << airside::to_string(task.service)
                          << ' ' << airside::to_string(task.state)
                          << " wait=" << task.waiting.count() << " sec requires=" << task.required_resource
                          << " resource=" << task.assigned_resource << '\n';
            }
        }
        std::cout << '\n';
    } else std::cout << '\n';
    const auto simulated_seconds = static_cast<double>(result.simulated_duration.count());
    const auto speed = execution_seconds > 0.0 ? simulated_seconds / execution_seconds : 0.0;
    std::cout << std::setprecision(6)
              << "Simulated time: " << simulated_seconds << " seconds\n"
              << "Execution time: " << execution_seconds << " seconds\n"
              << std::setprecision(0) << "Illustrative simulation speed: " << speed << "x real time\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        const auto options = parse_options(argc, argv);
        auto scenario = airside::load_scenario(options.scenario);
        const auto scenario_name = scenario.name;
        const auto seed = airside::resolve_seed(scenario, options.seed);
        airside::Simulation simulation{std::move(scenario), seed};
        ConsoleEventSink console_sink;
        std::optional<JsonLinesEventSink> recording_sink;
        if (options.verbose) simulation.add_event_sink(console_sink);
        if (options.event_recording) {
            recording_sink.emplace(*options.event_recording);
            simulation.add_event_sink(*recording_sink);
        }

        const auto started = std::chrono::steady_clock::now();
        while (simulation.advance()) {
            if (options.dump_snapshots) print_snapshot(simulation.snapshot());
        }
        const auto result = simulation.result();
        if (options.metrics_json || options.metrics_csv) {
            write_metrics(options.metrics_json.value_or(std::filesystem::path{}),
                options.metrics_csv.value_or(std::filesystem::path{}), result, simulation.snapshot());
        }
        const auto elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started).count();
        if (console_sink.failed() || (recording_sink && recording_sink->failed())) {
            throw std::runtime_error("an output sink failed while processing simulation events");
        }
        print_report(result, scenario_name, elapsed);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "airside_cli: " << error.what() << '\n';
        print_usage();
        return 1;
    }
}
