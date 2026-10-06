#include "airside/operations/simulation.hpp"
#include "airside/scenario/scenario_loader.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <algorithm>
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
    std::optional<std::filesystem::path> export_run_directory;
    bool verbose{true};
    bool dump_snapshots{false};
    bool disable_road_events{false};
};

void print_usage() {
    std::cout
        << "Usage: airside_cli [--scenario FILE] [--seed NUMBER] [--quiet]\n"
        << "                   [--dump-snapshots] [--record-events FILE]\n"
        << "                   [--export-run-dir DIR] [--disable-road-events] [--help]\n";
}

Options parse_options(int argc, char* argv[]) {
    Options result;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--help") { print_usage(); std::exit(0); }
        if (argument == "--quiet") { result.verbose = false; continue; }
        if (argument == "--dump-snapshots") { result.dump_snapshots = true; continue; }
        if (argument == "--disable-road-events") { result.disable_road_events = true; continue; }
        if (argument == "--scenario" || argument == "--record-events" ||
            argument == "--export-run-dir" || argument == "--seed") {
            if (++index >= argc) throw std::invalid_argument(std::format("{} requires a value", argument));
            const std::string value{argv[index]};
            if (argument == "--scenario") result.scenario = value;
            else if (argument == "--record-events") result.event_recording = value;
            else if (argument == "--export-run-dir") result.export_run_directory = value;
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

std::string csv_escape(std::string_view text) {
    if (text.find_first_of(",\"\n\r") == std::string_view::npos) return std::string{text};
    std::string result{"\""};
    for (const char character : text) result += character == '"' ? "\"\"" : std::string(1, character);
    return result + '"';
}

std::string optional_seconds(const std::optional<airside::SimTime>& time) {
    return time ? std::to_string(time->count()) : std::string{};
}

void write_run_bundle(
    const std::filesystem::path& directory,
    const airside::SimulationResult& result,
    const airside::AirportGraph& graph,
    std::string_view scenario_name,
    std::string_view source_scenario,
    double execution_ms) {
    std::filesystem::create_directories(directory);
    const auto csv_path = directory / "runs.csv";
    const auto aircraft_path = directory / "aircraft.csv";
    const auto events_path = directory / "events.jsonl";
    const auto json_path = directory / "experiment.json";

    double delay_total = 0.0;
    double waiting_total = 0.0;
    std::size_t completed_tasks = 0;
    std::size_t task_count = 0;
    for (const auto& aircraft : result.aircraft) {
        for (const auto& task : aircraft.tasks()) {
            ++task_count;
            if (task.status == airside::TaskStatus::Completed) ++completed_tasks;
        }
    }
    for (const auto& aircraft : result.metrics.aircraft) {
        delay_total += static_cast<double>(aircraft.departure_delay.count());
        waiting_total += static_cast<double>(aircraft.service_waiting.count());
    }
    const auto aircraft_count = result.metrics.aircraft.size();
    const double average_delay_seconds = aircraft_count ? delay_total / static_cast<double>(aircraft_count) : 0.0;
    const double average_waiting_seconds = aircraft_count ? waiting_total / static_cast<double>(aircraft_count) : 0.0;
    const auto disruption_events = static_cast<std::size_t>(std::ranges::count_if(result.events, [](const auto& event) {
        return event.type == airside::SimulationEventType::RoadClosed ||
               event.type == airside::SimulationEventType::RoadOpened;
    }));
    const auto road_closure_events = static_cast<std::size_t>(std::ranges::count_if(result.events, [](const auto& event) {
        return event.type == airside::SimulationEventType::RoadClosed;
    }));

    std::ofstream runs{csv_path};
    runs << "ordinal,case_id,seed,replication,scenario,simulated_duration_seconds,avg_turnaround_minutes,"
            "avg_departure_delay_minutes,avg_service_waiting_minutes,delayed_aircraft,aircraft_count,"
            "fuel_utilization,baggage_utilization,service_task_count,completed_service_tasks,disruption_events,road_closure_events,event_count,execution_ms\n"
         << std::fixed << std::setprecision(6)
         << "1,case_0001," << result.seed << ",1," << csv_escape(scenario_name) << ','
         << result.simulated_duration.count() << ',' << result.metrics.average_turnaround_seconds / 60.0 << ','
         << average_delay_seconds / 60.0 << ',' << average_waiting_seconds / 60.0 << ','
         << result.metrics.delayed_aircraft << ',' << aircraft_count << ',' << result.metrics.fuel_utilization << ','
         << result.metrics.baggage_utilization << ',' << task_count << ',' << completed_tasks << ','
         << disruption_events << ',' << road_closure_events << ',' << result.events.size() << ',' << execution_ms << '\n';
    if (!runs) throw std::runtime_error("failed to write runs.csv");

    std::ofstream aircraft_file{aircraft_path};
    aircraft_file << "aircraft_id,flight_number,state,scheduled_arrival_seconds,scheduled_departure_seconds,"
                     "actual_arrival_seconds,actual_departure_seconds,turnaround_duration_seconds,"
                     "departure_delay_seconds,service_waiting_seconds,task_count,completed_tasks\n"
                  << std::fixed << std::setprecision(6);
    for (const auto& aircraft : result.aircraft) {
        const auto metrics = std::ranges::find(result.metrics.aircraft, aircraft.id(), &airside::AircraftMetrics::id);
        const auto turnaround = aircraft.actual_arrival() && aircraft.actual_departure()
            ? std::to_string((*aircraft.actual_departure() - *aircraft.actual_arrival()).count()) : std::string{};
        const auto delay = metrics == result.metrics.aircraft.end() ? std::string{} :
            std::to_string(metrics->departure_delay.count());
        const auto waiting = metrics == result.metrics.aircraft.end() ? std::string{} :
            std::to_string(metrics->service_waiting.count());
        const auto completed = static_cast<std::size_t>(std::ranges::count_if(aircraft.tasks(), [](const auto& task) {
            return task.status == airside::TaskStatus::Completed;
        }));
        aircraft_file << aircraft.id().value() << ',' << csv_escape(aircraft.flight_number()) << ','
                      << airside::to_string(aircraft.state()) << ',' << aircraft.scheduled_arrival().count() << ','
                      << aircraft.scheduled_departure().count() << ',' << optional_seconds(aircraft.actual_arrival()) << ','
                      << optional_seconds(aircraft.actual_departure()) << ',' << turnaround << ',' << delay << ','
                      << waiting << ',' << aircraft.tasks().size() << ',' << completed << '\n';
    }
    if (!aircraft_file) throw std::runtime_error("failed to write aircraft.csv");

    std::ofstream events{events_path};
    for (const auto& event : result.events) {
        events << "{\"sequence\":" << event.sequence << ",\"time_seconds\":" << event.timestamp.count()
               << ",\"type\":\"" << airside::to_string(event.type) << '"';
        if (event.aircraft) events << ",\"aircraft_id\":" << event.aircraft->value();
        if (!event.aircraft_name.empty()) events << ",\"aircraft_name\":\"" << json_escape(event.aircraft_name) << '"';
        if (event.vehicle) events << ",\"vehicle_id\":" << event.vehicle->value();
        if (!event.vehicle_name.empty()) events << ",\"vehicle_name\":\"" << json_escape(event.vehicle_name) << '"';
        if (event.gate) events << ",\"gate_id\":" << event.gate->value();
        if (event.edge) {
            events << ",\"edge_id\":" << event.edge->value();
            const auto& edge = graph.edge(*event.edge);
            const auto action = event.type == airside::SimulationEventType::RoadClosed ? "Closure" : "Reopened";
            events << ",\"detail\":\"" << action << ": "
                   << json_escape(graph.node(edge.from).name) << " -> "
                   << json_escape(graph.node(edge.to).name) << '"';
        }
        if (event.service) events << ",\"service\":\"" << airside::to_string(*event.service) << '"';
        if (event.previous_aircraft_state) events << ",\"previous_aircraft_state\":\"" << airside::to_string(*event.previous_aircraft_state) << '"';
        if (event.aircraft_state) events << ",\"aircraft_state\":\"" << airside::to_string(*event.aircraft_state) << '"';
        if (event.previous_vehicle_state) events << ",\"previous_vehicle_state\":\"" << airside::to_string(*event.previous_vehicle_state) << '"';
        if (event.vehicle_state) events << ",\"vehicle_state\":\"" << airside::to_string(*event.vehicle_state) << '"';
        if (event.route) {
            events << ",\"route_nodes\":[";
            for (std::size_t index = 0; index < event.route->nodes.size(); ++index) {
                if (index) events << ',';
                events << event.route->nodes[index].value();
            }
            events << "],\"route_node_names\":[";
            for (std::size_t index = 0; index < event.route->nodes.size(); ++index) {
                if (index) events << ',';
                events << '"' << json_escape(graph.node(event.route->nodes[index]).name) << '"';
            }
            events << ']';
            if (!event.route->nodes.empty()) {
                std::string route_detail{"Route: "};
                for (std::size_t index = 0; index < event.route->nodes.size(); ++index) {
                    if (index) route_detail += " -> ";
                    route_detail += graph.node(event.route->nodes[index]).name;
                }
                events << ",\"detail\":\"" << json_escape(route_detail) << '"';
            }
        }
        events << "}\n";
    }
    if (!events) throw std::runtime_error("failed to write events.jsonl");

    const auto completed_at = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    const auto timestamp = std::format("{:%FT%TZ}", completed_at);
    std::ofstream metadata{json_path};
    metadata << std::fixed << std::setprecision(6)
        << "{\n  \"schema_version\": 1,\n  \"experiment_name\": \"" << json_escape(scenario_name)
        << "\",\n  \"source_scenario\": \"" << json_escape(source_scenario)
        << "\",\n  \"scenario_name\": \"" << json_escape(scenario_name)
        << "\",\n  \"seed\": " << result.seed << ",\n  \"completed_at_utc\": \"" << timestamp
        << "\",\n  \"run_count\": 1,\n  \"runs\": [{\"ordinal\":1,\"case_id\":\"case_0001\",\"seed\":"
        << result.seed << ",\"replication\":1,\"scenario\":\"" << json_escape(scenario_name)
        << "\",\"simulated_duration_seconds\":" << result.simulated_duration.count()
        << ",\"avg_turnaround_minutes\":" << result.metrics.average_turnaround_seconds / 60.0
        << ",\"avg_departure_delay_minutes\":" << average_delay_seconds / 60.0
        << ",\"avg_service_waiting_minutes\":" << average_waiting_seconds / 60.0
        << ",\"delayed_aircraft\":" << result.metrics.delayed_aircraft << ",\"aircraft_count\":" << aircraft_count
        << ",\"fuel_utilization\":" << result.metrics.fuel_utilization
        << ",\"baggage_utilization\":" << result.metrics.baggage_utilization
        << ",\"service_task_count\":" << task_count << ",\"completed_service_tasks\":" << completed_tasks
        << ",\"disruption_events\":" << disruption_events << ",\"road_closure_events\":" << road_closure_events
        << ",\"event_count\":" << result.events.size() << ",\"execution_ms\":" << execution_ms << "}],\n"
        << "  \"outputs\": [\"runs.csv\", \"aircraft.csv\", \"events.jsonl\"]\n}\n";
    if (!metadata) throw std::runtime_error("failed to write experiment.json");
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
            if (event.edge) output_ << ",\"edge_id\":" << event.edge->value();
            if (event.service) output_ << ",\"service\":\"" << airside::to_string(*event.service) << '"';
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
}

double minutes(airside::SimTime value) { return static_cast<double>(value.count()) / 60.0; }

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
              << result.metrics.aircraft.size() << "\n\n";
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
        if (options.disable_road_events) {
            scenario.road_events.clear();
            scenario.name += "_no_road_events";
        }
        const auto scenario_name = scenario.name;
        const auto scenario_graph = scenario.graph;
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
        if (options.export_run_directory) {
            write_run_bundle(*options.export_run_directory, result, scenario_graph, scenario_name,
                options.scenario.lexically_normal().generic_string(), elapsed * 1000.0);
        }
        print_report(result, scenario_name, elapsed);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "airside_cli: " << error.what() << '\n';
        print_usage();
        return 1;
    }
}
