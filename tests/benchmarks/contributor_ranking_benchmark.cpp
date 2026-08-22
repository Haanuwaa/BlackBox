#include "analysis/contributor_ranker.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <vector>

namespace analysis = blackbox::analysis;
namespace core = blackbox::core;
using namespace std::chrono_literals;

namespace {

struct Fixture {
    std::shared_ptr<const core::IncidentSnapshot> incident{};
    analysis::IncidentAnalysis analysis{};
};

[[nodiscard]] Fixture fixture(const std::size_t process_count) {
    constexpr std::size_t frames = 150U;
    const auto ranked_count = (std::min)(process_count, std::size_t{100U});
    core::IncidentHeader header{};
    header.window.event_time = core::MonotonicTimePoint{120s};
    header.window.requested_start = core::MonotonicTimePoint{0s};
    header.window.requested_end = core::MonotonicTimePoint{149s};
    header.actual_start = header.window.requested_start;
    header.actual_end = header.window.requested_end;
    std::vector<core::IncidentProcessSample> samples;
    samples.reserve(process_count * frames);
    for (std::size_t frame = 0U; frame < frames; ++frame) {
        for (std::size_t process = 0U; process < process_count; ++process) {
            const auto anomalous = process < ranked_count &&
                (process % 2U == 0U ? frame >= 112U && frame <= 121U
                                    : frame >= 124U && frame <= 133U);
            core::IncidentProcessSample sample{};
            sample.observed_at = core::MonotonicTimePoint{
                std::chrono::seconds{static_cast<std::int64_t>(frame)}};
            sample.identity = {static_cast<std::uint32_t>(process + 1U),
                               process + 1'000U};
            sample.cpu_fraction = {anomalous ? 0.80 : 0.02,
                                   core::RecordedValueStatus::available};
            samples.push_back(sample);
        }
    }
    Fixture result{};
    result.incident = std::make_shared<const core::IncidentSnapshot>(
        std::move(header), std::vector<core::IncidentSystemSample>{},
        std::vector<core::IncidentProcessInfo>{}, std::move(samples));
    result.analysis.evaluation_start = core::MonotonicTimePoint{90s};
    result.analysis.evaluation_end = core::MonotonicTimePoint{149s};
    analysis::ResourceAnomaly cpu_pressure{};
    cpu_pressure.resource = analysis::ResourceKind::cpu;
    cpu_pressure.score = 1.0;
    cpu_pressure.statistical_score = 1.0;
    cpu_pressure.pressure_metric = analysis::MetricKind::system_cpu;
    cpu_pressure.confidence = analysis::AnalysisConfidence::high;
    cpu_pressure.uncontextualized_score = 1.0;
    result.analysis.resources.push_back(std::move(cpu_pressure));
    result.analysis.processes.reserve(ranked_count);
    for (std::size_t process = 0U; process < ranked_count; ++process) {
        analysis::MetricAnomalyEvidence evidence{};
        evidence.metric = analysis::MetricKind::process_cpu;
        evidence.availability = analysis::EvidenceAvailability::available;
        evidence.direction = analysis::AnomalyDirection::higher;
        evidence.score = 1.0;
        evidence.robust_z = 100.0;
        evidence.observed_value = 0.80;
        evidence.baseline.sample_count = 60U;
        evidence.baseline.median = 0.02;
        evidence.baseline.p05 = 0.019;
        evidence.baseline.p95 = 0.021;
        evidence.baseline.robust_scale = 0.001;
        evidence.evaluation_samples = 60U;
        analysis::ProcessAnomaly anomaly{};
        anomaly.identity = {static_cast<std::uint32_t>(process + 1U),
                            process + 1'000U};
        anomaly.name = "candidate";
        anomaly.score = 1.0;
        anomaly.confidence = analysis::AnalysisConfidence::high;
        anomaly.evidence.push_back(evidence);
        result.analysis.processes.push_back(std::move(anomaly));
    }
    return result;
}

void measure(const std::size_t process_count) {
    constexpr std::size_t trials = 20U;
    const auto input = fixture(process_count);
    std::vector<double> durations;
    durations.reserve(trials);
    std::size_t output_count{};
    for (std::size_t trial = 0U; trial < trials; ++trial) {
        const auto started = std::chrono::steady_clock::now();
        const auto result = analysis::rank_contributors(*input.incident, input.analysis);
        durations.push_back(std::chrono::duration<double, std::milli>{
            std::chrono::steady_clock::now() - started}.count());
        output_count = result.size();
    }
    std::sort(durations.begin(), durations.end());
    const auto percentile = [&](const std::size_t numerator) {
        const auto rank = (durations.size() * numerator + 99U) / 100U;
        return durations[(std::max<std::size_t>)(1U, rank) - 1U];
    };
    const auto average = std::accumulate(durations.begin(), durations.end(), 0.0) /
                         static_cast<double>(durations.size());
    std::cout << process_count << ',' << process_count * 150U << ',' << average << ','
              << percentile(95U) << ',' << percentile(99U) << ',' << durations.back()
              << ',' << output_count << '\n';
}

} // namespace

int main() {
    std::cout << std::fixed << std::setprecision(3);
    std::cout << "processes,process_samples,average_ms,p95_ms,p99_ms,maximum_ms,"
                 "ranked_contributors\n";
    measure(50U);
    measure(200U);
    measure(500U);
}
