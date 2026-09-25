#include "airside/experiment/result.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace airside::experiment {
namespace {

double nearest_rank(const std::vector<double>& sorted, double probability) {
    const auto rank = static_cast<std::size_t>(std::ceil(probability * static_cast<double>(sorted.size())));
    return sorted[std::clamp(rank, std::size_t{1}, sorted.size()) - 1];
}

template <typename Projection>
DistributionStatistics project_statistics(const std::vector<const RunResult*>& runs, Projection projection) {
    std::vector<double> values;
    values.reserve(runs.size());
    for (const auto* run : runs) values.push_back(projection(*run));
    return calculate_statistics(std::move(values));
}

}  // namespace

DistributionStatistics calculate_statistics(std::vector<double> values) {
    if (values.empty()) throw std::invalid_argument("statistics require at least one value");
    double mean = 0.0;
    double m2 = 0.0;
    std::size_t count = 0;
    for (const auto value : values) {
        ++count;
        const auto delta = value - mean;
        mean += delta / static_cast<double>(count);
        m2 += delta * (value - mean);
    }
    std::ranges::sort(values);
    const auto middle = values.size() / 2;
    const auto median = values.size() % 2 == 0 ? (values[middle - 1] + values[middle]) / 2.0 : values[middle];
    return {values.size(), mean, values.front(), values.back(),
        std::sqrt(m2 / static_cast<double>(values.size())), median,
        nearest_rank(values, 0.50), nearest_rank(values, 0.90), nearest_rank(values, 0.95)};
}

std::vector<CaseSummary> summarize_results(
    const std::vector<ExperimentCase>& cases,
    const std::vector<RunResult>& runs) {
    std::vector<CaseSummary> summaries;
    summaries.reserve(cases.size());
    for (const auto& experiment_case : cases) {
        std::vector<const RunResult*> matching;
        for (const auto& run : runs) if (run.case_id == experiment_case.id) matching.push_back(&run);
        if (matching.empty()) throw std::invalid_argument("cannot summarize an experiment case with no runs");
        double delayed_total = 0.0;
        std::size_t any_delay = 0;
        for (const auto* run : matching) {
            delayed_total += static_cast<double>(run->delayed_aircraft);
            any_delay += run->delayed_aircraft > 0 ? 1U : 0U;
        }
        summaries.push_back({experiment_case,
            project_statistics(matching, [](const auto& run) { return run.average_turnaround_seconds; }),
            project_statistics(matching, [](const auto& run) { return run.average_departure_delay_seconds; }),
            project_statistics(matching, [](const auto& run) { return run.average_service_waiting_seconds; }),
            project_statistics(matching, [](const auto& run) { return run.fuel_utilization; }),
            project_statistics(matching, [](const auto& run) { return run.baggage_utilization; }),
            delayed_total / static_cast<double>(matching.size()),
            static_cast<double>(any_delay) / static_cast<double>(matching.size())});
    }
    return summaries;
}

}  // namespace airside::experiment
