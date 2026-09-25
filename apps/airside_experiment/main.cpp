#include "airside/experiment/executor.hpp"
#include "airside/experiment/experiment_loader.hpp"
#include "airside/experiment/writer.hpp"
#include "airside/scenario/scenario_loader.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <iomanip>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace {

enum class SortMetric { CaseId, MeanTurnaround, P95Turnaround, ProbabilityAnyDelay };

struct Options {
    std::filesystem::path experiment;
    std::optional<std::size_t> workers;
    std::optional<std::filesystem::path> output;
    SortMetric sort{SortMetric::CaseId};
    bool quiet{false};
    bool dry_run{false};
};

void print_usage() {
    std::cout
        << "Usage: airside_experiment --experiment FILE [--workers N] [--output DIR]\n"
        << "                          [--sort case|mean-turnaround|p95-turnaround|delay-probability]\n"
        << "                          [--quiet] [--dry-run] [--help]\n";
}

std::size_t parse_positive_size(std::string_view text, std::string_view option) {
    std::size_t consumed = 0;
    const auto result = std::stoull(std::string{text}, &consumed);
    if (consumed != text.size() || result == 0) {
        throw std::invalid_argument(std::format("{} requires a positive integer", option));
    }
    return static_cast<std::size_t>(result);
}

Options parse_options(int argc, char* argv[]) {
    Options result;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--help") { print_usage(); std::exit(0); }
        if (argument == "--quiet") { result.quiet = true; continue; }
        if (argument == "--dry-run") { result.dry_run = true; continue; }
        if (argument == "--experiment" || argument == "--workers" || argument == "--output" || argument == "--sort") {
            if (++index >= argc) throw std::invalid_argument(std::format("{} requires a value", argument));
            const std::string value{argv[index]};
            if (argument == "--experiment") result.experiment = value;
            else if (argument == "--workers") result.workers = parse_positive_size(value, argument);
            else if (argument == "--output") result.output = value;
            else if (value == "case") result.sort = SortMetric::CaseId;
            else if (value == "mean-turnaround") result.sort = SortMetric::MeanTurnaround;
            else if (value == "p95-turnaround") result.sort = SortMetric::P95Turnaround;
            else if (value == "delay-probability") result.sort = SortMetric::ProbabilityAnyDelay;
            else throw std::invalid_argument("unknown sort metric");
            continue;
        }
        throw std::invalid_argument(std::format("unknown argument: {}", argument));
    }
    if (result.experiment.empty()) throw std::invalid_argument("--experiment is required");
    return result;
}

void print_definition(
    const airside::experiment::ExperimentDefinition& definition,
    const std::vector<airside::experiment::ExperimentCase>& cases,
    std::size_t runs,
    std::size_t workers) {
    std::cout << "RampLab Experiment: " << definition.name << "\n\n"
              << "Scenario: " << definition.scenario_path.string() << "\n"
              << "Configurations: " << cases.size() << "\n"
              << "Seeds: " << definition.seeds.values.size() << "\n"
              << "Replications per seed: " << definition.seeds.replications << "\n"
              << "Total simulations: " << runs << "\n"
              << "Workers: " << workers;
    if (definition.workers.automatic) std::cout << " (auto policy)";
    std::cout << "\nHardware concurrency: " << std::thread::hardware_concurrency() << "\n\nParameters:\n";
    for (const auto& axis : definition.parameters) {
        std::cout << "  " << airside::experiment::parameter_name(axis.key) << ": ";
        for (std::size_t index = 0; index < axis.values.size(); ++index) {
            if (index != 0) std::cout << ", ";
            std::cout << airside::experiment::format_parameter_value(axis.values[index]);
        }
        std::cout << '\n';
    }
}

void print_comparison(
    std::vector<airside::experiment::CaseSummary> summaries,
    SortMetric sort) {
    const auto compare = [sort](const auto& left, const auto& right) {
        switch (sort) {
        case SortMetric::MeanTurnaround:
            return left.average_turnaround_seconds.mean < right.average_turnaround_seconds.mean;
        case SortMetric::P95Turnaround:
            return left.average_turnaround_seconds.p95 < right.average_turnaround_seconds.p95;
        case SortMetric::ProbabilityAnyDelay:
            return left.probability_any_delay < right.probability_any_delay;
        case SortMetric::CaseId: return left.experiment_case.id < right.experiment_case.id;
        }
        return false;
    };
    std::ranges::stable_sort(summaries, compare);
    std::cout << "\nComparison\n"
              << std::left << std::setw(12) << "Case"
              << std::right << std::setw(18) << "Avg turnaround"
              << std::setw(18) << "P95 turnaround"
              << std::setw(16) << "P(any delay)" << '\n';
    for (const auto& item : summaries) {
        std::cout << std::left << std::setw(12) << item.experiment_case.id
                  << std::right << std::fixed << std::setprecision(2)
                  << std::setw(14) << item.average_turnaround_seconds.mean / 60.0 << " min"
                  << std::setw(14) << item.average_turnaround_seconds.p95 / 60.0 << " min"
                  << std::setw(15) << item.probability_any_delay * 100.0 << "%\n";
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        const auto options = parse_options(argc, argv);
        auto definition = airside::experiment::load_experiment(options.experiment);
        const auto cases = airside::experiment::generate_cases(definition);
        const auto requests = airside::experiment::generate_runs(definition, cases);
        const auto worker_count = options.workers.value_or(
            definition.workers.automatic ? airside::experiment::automatic_worker_count() : definition.workers.count);
        if (!options.quiet) print_definition(definition, cases, requests.size(), worker_count);
        if (options.dry_run) {
            if (!options.quiet) std::cout << "\nNo simulations executed.\n";
            return 0;
        }

        const auto scenario = airside::load_scenario(definition.scenario_path);
        auto last_progress = std::chrono::steady_clock::now() - std::chrono::seconds{2};
        const auto progress = options.quiet ? airside::experiment::ProgressCallback{} :
            airside::experiment::ProgressCallback{[&](const auto& status) {
                const auto now = std::chrono::steady_clock::now();
                if (status.completed != status.total && now - last_progress < std::chrono::seconds{1}) return;
                last_progress = now;
                const auto rate = status.elapsed.count() > 0.0
                    ? static_cast<double>(status.completed) / status.elapsed.count() : 0.0;
                std::cout << std::fixed << std::setprecision(1)
                          << "Runs: " << status.completed << " / " << status.total
                          << "  Progress: " << (100.0 * static_cast<double>(status.completed) / static_cast<double>(status.total)) << "%"
                          << "  Elapsed: " << std::setprecision(2) << status.elapsed.count() << "s"
                          << "  Rate: " << std::setprecision(1) << rate << " simulations/sec\n";
            }};
        const auto execution = airside::experiment::execute_runs(scenario, requests, worker_count, progress);
        const auto summaries = airside::experiment::summarize_results(cases, execution.runs);
        const auto output = options.output.value_or(definition.output_directory);
        airside::experiment::write_experiment_outputs(definition, cases, execution, summaries,
            output, std::chrono::system_clock::now(), AIRSIDE_PROJECT_VERSION);
        if (!options.quiet) {
            print_comparison(summaries, options.sort);
            std::cout << "\nCompleted " << execution.runs.size() << " simulations in "
                      << std::fixed << std::setprecision(3) << execution.wall_time.count() << "s ("
                      << std::setprecision(1) << static_cast<double>(execution.runs.size()) / execution.wall_time.count()
                      << " simulations/sec).\nOutputs: " << output.string() << '\n';
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "airside_experiment: " << error.what() << '\n';
        print_usage();
        return 1;
    }
}
