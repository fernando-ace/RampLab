#include "airside/experiment/writer.hpp"

#include <chrono>
#include <format>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace airside::experiment {
namespace {

std::string csv_escape(std::string_view value) {
    if (value.find_first_of(",\"\n\r") == std::string_view::npos) return std::string{value};
    std::string result{"\""};
    for (const auto character : value) result += character == '"' ? "\"\"" : std::string(1, character);
    return result + '"';
}

std::string json_escape(std::string_view value) {
    std::string result;
    for (const auto character : value) {
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

std::string iso_timestamp(std::chrono::system_clock::time_point value) {
    return std::format("{:%FT%TZ}", std::chrono::floor<std::chrono::seconds>(value));
}

void require_stream(const std::ofstream& stream, const std::filesystem::path& path) {
    if (!stream) throw std::runtime_error(std::format("failed to write '{}'", path.string()));
}

void write_distribution(std::ofstream& output, const DistributionStatistics& stats) {
    output << ',' << stats.mean << ',' << stats.minimum << ',' << stats.maximum
           << ',' << stats.standard_deviation << ',' << stats.median
           << ',' << stats.p50 << ',' << stats.p90 << ',' << stats.p95;
}

}  // namespace

void write_experiment_outputs(
    const ExperimentDefinition& definition,
    const std::vector<ExperimentCase>& cases,
    const ExecutionReport& execution,
    const std::vector<CaseSummary>& summaries,
    const std::filesystem::path& output_directory,
    std::chrono::system_clock::time_point completed_at,
    std::string_view revision) {
    std::filesystem::create_directories(output_directory);
    const auto runs_path = output_directory / "runs.csv";
    const auto summary_path = output_directory / "summary.csv";
    const auto metadata_path = output_directory / "experiment.json";

    std::ofstream runs{runs_path};
    runs << "run_ordinal,case_id,seed,replication,scenario";
    for (const auto& axis : definition.parameters) runs << ',' << parameter_name(axis.key);
    runs << ",simulated_duration_seconds,avg_turnaround_minutes,avg_departure_delay_minutes,avg_service_waiting_minutes,delayed_aircraft,aircraft_count,fuel_utilization,baggage_utilization,execution_ms,total_turnarounds,completed_turnarounds,delayed_turnarounds,failed_or_timed_out_turnarounds,maximum_turnaround_seconds,maximum_departure_delay_seconds,on_time_departures,on_time_departure_rate,total_service_task_wait_seconds,maximum_service_task_wait_seconds,task_reassignments,disruption_triggered_replans,unresolved_service_requests,service_resource_utilization,turnaround_task_timings\n";
    runs << std::fixed << std::setprecision(6);
    for (const auto& run : execution.runs) {
        runs << run.ordinal + 1 << ',' << csv_escape(run.case_id) << ',' << run.seed << ','
             << run.replication << ',' << csv_escape(run.scenario_name);
        for (const auto& parameter : run.parameters) runs << ',' << csv_escape(format_parameter_value(parameter.second));
        runs << ',' << run.simulated_duration_seconds
             << ',' << run.average_turnaround_seconds / 60.0
             << ',' << run.average_departure_delay_seconds / 60.0
             << ',' << run.average_service_waiting_seconds / 60.0
             << ',' << run.delayed_aircraft << ',' << run.aircraft_count
             << ',' << run.fuel_utilization << ',' << run.baggage_utilization
             << ',' << run.execution_ms << ',' << run.total_turnarounds << ',' << run.completed_turnarounds
             << ',' << run.delayed_turnarounds << ',' << run.failed_or_timed_out_turnarounds
             << ',' << run.maximum_turnaround_seconds << ',' << run.maximum_departure_delay_seconds
             << ',' << run.on_time_departures << ',' << run.on_time_departure_rate
             << ',' << run.total_service_task_wait_seconds << ',' << run.maximum_service_task_wait_seconds
             << ',' << run.task_reassignments << ',' << run.disruption_triggered_replans
             << ',' << run.unresolved_service_requests << ',';
        std::string resource_utilization;
        for (const auto& [type, value] : run.resource_utilization) {
            if (!resource_utilization.empty()) resource_utilization += ';';
            resource_utilization += type + ':' + std::format("{:.6f}", value);
        }
        std::string task_timings;
        for (const auto& aircraft : run.aircraft) {
            for (const auto& task : aircraft.tasks) {
                if (!task_timings.empty()) task_timings += ';';
                task_timings += aircraft.turnaround_id + '/' + std::to_string(task.task_id) + ':' +
                    task.service_type + ':' + task.state + ':' + std::to_string(task.requested_at_seconds) + ':' +
                    std::to_string(task.started_at_seconds) + ':' + std::to_string(task.completed_at_seconds) + ':' +
                    std::to_string(task.waiting_seconds) + ':' + task.required_resource + ':' + task.assigned_resource;
            }
        }
        runs << csv_escape(resource_utilization) << ',' << csv_escape(task_timings) << '\n';
    }
    require_stream(runs, runs_path);

    std::ofstream summary{summary_path};
    summary << "case_id";
    for (const auto& axis : definition.parameters) summary << ',' << parameter_name(axis.key);
    for (const auto prefix : {"turnaround_minutes", "departure_delay_minutes", "service_waiting_minutes"}) {
        summary << ',' << prefix << "_count," << prefix << "_mean," << prefix << "_min," << prefix
                << "_max," << prefix << "_stddev," << prefix << "_median," << prefix
                << "_p50," << prefix << "_p90," << prefix << "_p95";
    }
    summary << ",fuel_utilization_mean,baggage_utilization_mean,mean_delayed_aircraft,probability_any_delay\n";
    summary << std::fixed << std::setprecision(6);
    for (const auto& item : summaries) {
        summary << csv_escape(item.experiment_case.id);
        for (const auto& parameter : item.experiment_case.parameters) summary << ',' << csv_escape(format_parameter_value(parameter.second));
        const auto minutes = [](DistributionStatistics stats) {
            stats.mean /= 60.0; stats.minimum /= 60.0; stats.maximum /= 60.0;
            stats.standard_deviation /= 60.0; stats.median /= 60.0; stats.p50 /= 60.0;
            stats.p90 /= 60.0; stats.p95 /= 60.0; return stats;
        };
        const auto write = [&](const DistributionStatistics& stats) {
            summary << ',' << stats.count; write_distribution(summary, minutes(stats));
        };
        write(item.average_turnaround_seconds);
        write(item.average_departure_delay_seconds);
        write(item.average_service_waiting_seconds);
        summary << ',' << item.fuel_utilization.mean << ',' << item.baggage_utilization.mean
                << ',' << item.mean_delayed_aircraft << ',' << item.probability_any_delay << '\n';
    }
    require_stream(summary, summary_path);

    std::ofstream metadata{metadata_path};
    metadata << "{\n"
             << "  \"schema_version\": 1,\n"
             << "  \"experiment_name\": \"" << json_escape(definition.name) << "\",\n"
             << "  \"source_experiment\": \"" << json_escape(definition.source_file.generic_string()) << "\",\n"
             << "  \"source_scenario\": \"" << json_escape(definition.scenario_path.generic_string()) << "\",\n"
             << "  \"completed_at_utc\": \"" << iso_timestamp(completed_at) << "\",\n"
             << "  \"revision\": \"" << json_escape(revision) << "\",\n"
             << "  \"hardware_concurrency\": " << std::thread::hardware_concurrency() << ",\n"
             << "  \"worker_count\": " << execution.worker_count << ",\n"
             << "  \"configuration_count\": " << cases.size() << ",\n"
             << "  \"seed_count\": " << definition.seeds.values.size() << ",\n"
             << "  \"replications\": " << definition.seeds.replications << ",\n"
             << "  \"run_count\": " << execution.runs.size() << ",\n"
             << "  \"execution_seconds\": " << std::fixed << std::setprecision(6) << execution.wall_time.count() << ",\n"
             << "  \"seeds\": {\"values\": [";
    for (std::size_t index = 0; index < definition.seeds.values.size(); ++index) {
        if (index != 0) metadata << ", ";
        metadata << definition.seeds.values[index];
    }
    metadata << "], \"replications\": " << definition.seeds.replications << "},\n"
             << "  \"parameters\": {\n";
    for (std::size_t axis_index = 0; axis_index < definition.parameters.size(); ++axis_index) {
        const auto& axis = definition.parameters[axis_index];
        metadata << "    \"" << parameter_name(axis.key) << "\": [";
        for (std::size_t value_index = 0; value_index < axis.values.size(); ++value_index) {
            if (value_index != 0) metadata << ", ";
            const auto value = format_parameter_value(axis.values[value_index]);
            if (std::holds_alternative<bool>(axis.values[value_index]) ||
                std::holds_alternative<std::int64_t>(axis.values[value_index]) ||
                std::holds_alternative<double>(axis.values[value_index])) metadata << value;
        }
        metadata << ']' << (axis_index + 1 == definition.parameters.size() ? "\n" : ",\n");
    }
    metadata << "  },\n  \"runs\": [\n";
    for (std::size_t run_index = 0; run_index < execution.runs.size(); ++run_index) {
        const auto& run = execution.runs[run_index];
        metadata << "    {\"ordinal\": " << run.ordinal + 1 << ", \"case_id\": \"" << json_escape(run.case_id)
                 << "\", \"seed\": " << run.seed << ", \"total_turnarounds\": " << run.total_turnarounds
                 << ", \"completed_turnarounds\": " << run.completed_turnarounds
                 << ", \"delayed_turnarounds\": " << run.delayed_turnarounds
                 << ", \"failed_or_timed_out_turnarounds\": " << run.failed_or_timed_out_turnarounds
                 << ", \"mean_turnaround_duration_seconds\": " << run.average_turnaround_seconds
                 << ", \"mean_departure_delay_seconds\": " << run.average_departure_delay_seconds
                 << ", \"maximum_turnaround_seconds\": " << run.maximum_turnaround_seconds
                 << ", \"maximum_departure_delay_seconds\": " << run.maximum_departure_delay_seconds
                 << ", \"on_time_departure_rate\": " << run.on_time_departure_rate
                 << ", \"total_service_task_wait_seconds\": " << run.total_service_task_wait_seconds
                 << ", \"maximum_service_task_wait_seconds\": " << run.maximum_service_task_wait_seconds
                 << ", \"task_reassignments\": " << run.task_reassignments
                 << ", \"disruption_triggered_replans\": " << run.disruption_triggered_replans
                 << ", \"unresolved_service_requests\": " << run.unresolved_service_requests
                 << ", \"resource_utilization\": {";
        for (std::size_t resource_index = 0; resource_index < run.resource_utilization.size(); ++resource_index) {
            if (resource_index != 0) metadata << ',';
            metadata << "\"" << json_escape(run.resource_utilization[resource_index].first) << "\": "
                     << run.resource_utilization[resource_index].second;
        }
        metadata << "}, \"turnarounds\": [";
        for (std::size_t aircraft_index = 0; aircraft_index < run.aircraft.size(); ++aircraft_index) {
            const auto& aircraft = run.aircraft[aircraft_index];
            if (aircraft_index != 0) metadata << ',';
            metadata << "{\"turnaround_id\": \"" << json_escape(aircraft.turnaround_id)
                     << "\", \"aircraft\": \"" << json_escape(aircraft.flight_number)
                     << "\", \"completion_seconds\": " << aircraft.actual_completion_time_seconds
                     << ", \"turnaround_duration_seconds\": " << aircraft.turnaround_seconds
                     << ", \"departure_delay_seconds\": " << aircraft.departure_delay_seconds
                     << ", \"estimated_ready_time_seconds\": " << aircraft.estimated_ready_time_seconds
                     << ", \"schedule_slack_seconds\": " << aircraft.schedule_slack_seconds
                     << ", \"critical_path_task_ids\": \"" << json_escape(aircraft.critical_path_task_ids)
                     << "\", \"tasks\": [";
            for (std::size_t task_index = 0; task_index < aircraft.tasks.size(); ++task_index) {
                const auto& task = aircraft.tasks[task_index];
                if (task_index != 0) metadata << ',';
                metadata << "{\"task_id\": " << task.task_id << ", \"service_type\": \""
                         << json_escape(task.service_type) << "\", \"state\": \"" << json_escape(task.state)
                         << "\", \"requested_at_seconds\": " << (task.requested_at_seconds < 0 ? "null" : std::to_string(task.requested_at_seconds))
                         << ", \"started_at_seconds\": " << (task.started_at_seconds < 0 ? "null" : std::to_string(task.started_at_seconds))
                         << ", \"completed_at_seconds\": " << (task.completed_at_seconds < 0 ? "null" : std::to_string(task.completed_at_seconds))
                         << ", \"waiting_seconds\": " << task.waiting_seconds << ", \"required_resource\": \""
                         << json_escape(task.required_resource) << "\", \"assigned_resource\": \""
                         << json_escape(task.assigned_resource) << "\"}";
            }
            metadata << "]}";
        }
        metadata << "]}" << (run_index + 1 == execution.runs.size() ? "\n" : ",\n");
    }
    metadata << "  ],\n  \"outputs\": [\"runs.csv\", \"summary.csv\"]\n}\n";
    require_stream(metadata, metadata_path);
}

}  // namespace airside::experiment
