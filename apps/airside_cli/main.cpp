#include "airside/operations/simulation.hpp"
#include "airside/world/baseline_scenario.hpp"

#include <chrono>
#include <cstdint>
#include <exception>
#include <format>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

struct Options {
    std::uint64_t seed{42};
    bool verbose{true};
};

void print_usage() {
    std::cout << "Usage: airside_cli [--scenario baseline] [--seed NUMBER] [--quiet] [--help]\n";
}

Options parse_options(int argc, char* argv[]) {
    Options result;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--help") {
            print_usage();
            std::exit(0);
        }
        if (argument == "--quiet") {
            result.verbose = false;
            continue;
        }
        if (argument == "--scenario") {
            if (++index >= argc || std::string_view{argv[index]} != "baseline") {
                throw std::invalid_argument("the only available scenario is 'baseline'");
            }
            continue;
        }
        if (argument == "--seed") {
            if (++index >= argc) {
                throw std::invalid_argument("--seed requires an unsigned integer");
            }
            std::size_t consumed = 0;
            const std::string text{argv[index]};
            result.seed = std::stoull(text, &consumed);
            if (consumed != text.size()) {
                throw std::invalid_argument("--seed requires an unsigned integer");
            }
            continue;
        }
        throw std::invalid_argument(std::format("unknown argument: {}", argument));
    }
    return result;
}

double minutes(airside::SimTime value) {
    return static_cast<double>(value.count()) / 60.0;
}

void print_report(const airside::SimulationResult& result, double execution_seconds) {
    std::cout << "\n=== Simulation Complete ===\n\n"
              << "Seed: " << result.seed << '\n'
              << "Simulated duration: " << airside::format_sim_time(result.simulated_duration) << "\n\n"
              << "Aircraft\n";

    for (const auto& aircraft : result.metrics.aircraft) {
        std::cout << aircraft.flight_number << '\n'
                  << "  Turnaround: " << std::fixed << std::setprecision(1)
                  << minutes(aircraft.turnaround) << " min\n"
                  << "  Departure delay: " << minutes(aircraft.departure_delay) << " min\n"
                  << "  Service waiting: " << minutes(aircraft.service_waiting) << " min\n";
    }

    std::cout << "\nFleet\n"
              << "Fuel truck utilization: " << std::setprecision(1)
              << result.metrics.fuel_utilization * 100.0 << "%\n"
              << "Baggage cart utilization: "
              << result.metrics.baggage_utilization * 100.0 << "%\n\n"
              << "Summary\n"
              << "Average turnaround: "
              << result.metrics.average_turnaround_seconds / 60.0 << " min\n"
              << "Delayed aircraft: " << result.metrics.delayed_aircraft << " / "
              << result.metrics.aircraft.size() << "\n\n";

    const auto simulated_seconds = static_cast<double>(result.simulated_duration.count());
    const auto speed = execution_seconds > 0.0 ? simulated_seconds / execution_seconds : 0.0;
    std::cout << std::setprecision(6)
              << "Simulated time: " << simulated_seconds << " seconds\n"
              << "Execution time: " << execution_seconds << " seconds\n"
              << std::setprecision(0)
              << "Simulation speed: " << speed << "x real time\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        const auto options = parse_options(argc, argv);
        const auto started = std::chrono::steady_clock::now();
        const auto result = airside::Simulation{
            airside::make_baseline_scenario(), options.seed, options.verbose}.run();
        const auto finished = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration<double>(finished - started).count();
        print_report(result, elapsed);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "airside_cli: " << error.what() << '\n';
        print_usage();
        return 1;
    }
}
