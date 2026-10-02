#include "airside/operations/simulation.hpp"
#include "airside/scenario/scenario_loader.hpp"

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
    bool verbose{true};
    bool dump_snapshots{false};
};

void print_usage() {
    std::cout
        << "Usage: airside_cli [--scenario FILE] [--seed NUMBER] [--quiet]\n"
        << "                   [--dump-snapshots] [--record-events FILE] [--help]\n";
}

Options parse_options(int argc, char* argv[]) {
    Options result;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--help") { print_usage(); std::exit(0); }
        if (argument == "--quiet") { result.verbose = false; continue; }
        if (argument == "--dump-snapshots") { result.dump_snapshots = true; continue; }
        if (argument == "--scenario" || argument == "--record-events" || argument == "--seed") {
            if (++index >= argc) throw std::invalid_argument(std::format("{} requires a value", argument));
            const std::string value{argv[index]};
            if (argument == "--scenario") result.scenario = value;
            else if (argument == "--record-events") result.event_recording = value;
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
        if (vehicle.journey) {
            std::cout << "  vehicle=" << vehicle.name
                      << " origin=" << vehicle.journey->origin.value()
                      << " destination=" << vehicle.journey->destination.value()
                      << " depart=" << vehicle.journey->departure_time.count()
                      << " arrive=" << vehicle.journey->expected_arrival_time.count()
                      << " segments=" << vehicle.journey->segments.size() << '\n';
        }
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
