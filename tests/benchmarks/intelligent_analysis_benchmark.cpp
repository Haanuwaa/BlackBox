#include "analysis/diagnosis_fixture.hpp"
#include "analysis/intelligent_incident_analyzer.hpp"
#include "analysis/personalized_process_analyzer.hpp"
#include "storage/test_incident.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <optional>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#endif

namespace analysis = blackbox::analysis;
namespace fixture = blackbox::test::diagnosis_fixture;
namespace storage = blackbox::storage;

namespace {

[[nodiscard]] std::uint64_t working_set_bytes() noexcept {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
            sizeof(counters)) != FALSE) {
        return static_cast<std::uint64_t>(counters.WorkingSetSize);
    }
#endif
    return 0U;
}

struct Measurement final {
    double component_average_ms{};
    double pipeline_average_ms{};
    double pipeline_p95_ms{};
    double pipeline_maximum_ms{};
    std::uint64_t peak_temporary_bytes{};
    bool deterministic{};
};

[[nodiscard]] Measurement measure(const std::size_t process_count) {
    constexpr std::size_t trials = 20U;
    const auto input = storage::test::scaled_incident(process_count, 150U, false);
    analysis::PersonalizedProcessAnalyzer components;
    analysis::IntelligentIncidentAnalyzer pipeline;
    static_cast<void>(components.analyze(*input));
    const auto reference = pipeline.analyze(*input);
    std::vector<double> component_durations;
    std::vector<double> pipeline_durations;
    component_durations.reserve(trials);
    pipeline_durations.reserve(trials);
    std::uint64_t peak_temporary{};
    bool deterministic = reference.has_value();
    for (std::size_t trial = 0U; trial < trials; ++trial) {
        auto started = std::chrono::steady_clock::now();
        static_cast<void>(components.analyze(*input));
        component_durations.push_back(std::chrono::duration<double, std::milli>{
            std::chrono::steady_clock::now() - started}.count());
        started = std::chrono::steady_clock::now();
        const auto result = pipeline.analyze(*input);
        pipeline_durations.push_back(std::chrono::duration<double, std::milli>{
            std::chrono::steady_clock::now() - started}.count());
        deterministic = deterministic && result.has_value() &&
                        reference.has_value() && *result == *reference;
    }
    constexpr std::size_t memory_trials = 5U;
    for (std::size_t trial = 0U; trial < memory_trials; ++trial) {
        const auto memory_before = working_set_bytes();
        std::atomic<bool> monitoring{true};
        std::atomic<std::uint64_t> peak{memory_before};
        std::jthread monitor{[&](const std::stop_token stop_token) {
            while (!stop_token.stop_requested() && monitoring.load()) {
                const auto observed = working_set_bytes();
                auto previous = peak.load();
                while (observed > previous &&
                       !peak.compare_exchange_weak(previous, observed)) {}
                std::this_thread::sleep_for(std::chrono::microseconds{250});
            }
        }};
        static_cast<void>(pipeline.analyze(*input));
        monitoring.store(false);
        monitor.request_stop();
        monitor.join();
        peak.store((std::max)(peak.load(), working_set_bytes()));
        peak_temporary = (std::max)(
            peak_temporary,
            peak.load() > memory_before ? peak.load() - memory_before : 0U);
    }
    std::sort(pipeline_durations.begin(), pipeline_durations.end());
    const auto average = [](const auto& values) {
        return std::accumulate(values.begin(), values.end(), 0.0) /
               static_cast<double>(values.size());
    };
    const auto p95_index = (pipeline_durations.size() * 95U + 99U) / 100U - 1U;
    return {average(component_durations), average(pipeline_durations),
            pipeline_durations[p95_index], pipeline_durations.back(),
            peak_temporary, deterministic};
}

} // namespace

int main(const int argument_count, const char* const* arguments) {
    std::cout << std::fixed << std::setprecision(3);
    std::cout << "processes,component_average_ms,pipeline_average_ms,pipeline_overhead_ms,"
                 "pipeline_p95_ms,pipeline_maximum_ms,peak_temporary_bytes,deterministic\n";
    for (const auto count : {50U, 200U, 500U}) {
        const auto result = measure(count);
        std::cout << count << ',' << result.component_average_ms << ','
                  << result.pipeline_average_ms << ','
                  << result.pipeline_average_ms - result.component_average_ms << ','
                  << result.pipeline_p95_ms << ',' << result.pipeline_maximum_ms << ','
                  << result.peak_temporary_bytes << ',' << result.deterministic << '\n';
    }

    std::size_t correct{};
    analysis::IntelligentIncidentAnalyzer pipeline;
    for (const auto resource : {analysis::ResourceKind::cpu,
                                analysis::ResourceKind::memory,
                                analysis::ResourceKind::disk,
                                analysis::ResourceKind::network}) {
        const auto input = fixture::incident(resource, std::nullopt, true);
        const auto result = pipeline.analyze(*input);
        correct += result && result->diagnosis.type == fixture::expected_type(resource);
    }
    const auto quiet = pipeline.analyze(*fixture::incident(std::nullopt));
    const auto quiet_false_positive = quiet && quiet->diagnosis.available;
    std::uintmax_t executable_bytes{};
    if (argument_count > 0) {
        std::error_code error;
        executable_bytes = std::filesystem::file_size(arguments[0], error);
        if (error) executable_bytes = 0U;
    }
    std::cout << "labeled_diagnoses_correct," << correct << "/4\n"
              << "quiet_false_positives," << quiet_false_positive << "/1\n"
              << "benchmark_executable_bytes," << executable_bytes << '\n'
              << "pipeline_version," << analysis::intelligent_pipeline_version << '\n'
              << "evidence_model_version," <<
                     analysis::diagnosis_evidence_model_version << '\n'
              << "configuration_fingerprint," <<
                     analysis::intelligent_configuration_fingerprint({}) << '\n'
              << "native_ml_adopted,0\n"
              << "native_ml_gate,NO_REPRESENTATIVE_DATASET_OR_MATERIAL_QUALITY_GAIN\n";
    return correct == 4U && !quiet_false_positive ? 0 : 1;
}
